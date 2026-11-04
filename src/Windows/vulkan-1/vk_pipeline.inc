/* vulkan-1.c, part 4: graphics pipelines. The shaders are SPIR-V (spirv.inc turns them into DXBC, which the kernel takes on every GPU); the resources the shaders use are numbered for the
 * whole pipeline here: constant buffer slots (the kernel lays them end to end in its push constant block) and texture units. */

static UINT put_words(UINT *w, UINT n, UINT v) {
  w[n] = v;
  return n + 1;
}

static BOOL create_kernel_shader(Device *d, const BYTE *dxbc, UINT bytes, UINT stage, KMT_HANDLE *out) {
  UINT padded = (bytes + 3) & ~3u;
  UINT total = 16 + padded;
  BYTE *req = vk_alloc(total);
  if (!req) return FALSE;
  put32(req, 0, 1);       /* CreateShader */
  put32(req, 4, 7);       /* ShaderKind::Dxbc */
  put32(req, 8, stage);   /* 0 vertex, 1 fragment */
  put32(req, 12, bytes);
  memcpy(req + 16, dxbc, bytes);
  NTSTATUS st = kmt_escape(d, req, total);
  if (STATUS_OK(st)) *out = get32(req, 0);
  vk_free(req);
  return STATUS_OK(st) && *out != 0;
}

typedef struct Stage {
  SpvModule *m;
  UINT stage;
} Stage;

static void add_cb_slot(Pipe *p, UINT kind, UINT set, UINT binding, UINT vec4) {
  for (UINT i = 0; i < p->ncb; ++i)
    if (p->cb[i].kind == kind && p->cb[i].set == set && p->cb[i].binding == binding) {
      p->cb[i].vec4 = MAX(p->cb[i].vec4, vec4);
      return;
    }
  if (p->ncb < 16) {
    CbSlot *c = &p->cb[p->ncb++];
    c->kind = kind, c->set = set, c->binding = binding, c->vec4 = vec4;
  }
}

/* Numbers the resources of the shaders of one pipeline. FALSE when the adapter cannot hold them. */
static BOOL assign_resources(Pipe *p, Stage *stages, UINT nstages, const Caps *caps, const char **why) {
  /* constant buffers: the uniform blocks by (set, binding), then the push constant block */
  for (UINT s = 0; s < nstages; ++s)
    for (UINT i = 0; i < spv_resource_count(stages[s].m); ++i) {
      SpvResource *r = spv_resource(stages[s].m, i);
      if (r->kind == SPV_RES_UNIFORM) add_cb_slot(p, SPV_RES_UNIFORM, r->set, r->binding, (r->bytes + 15) / 16);
    }
  for (UINT a = 1; a < p->ncb; ++a) {   /* (sorted: insertion) */
    CbSlot c = p->cb[a];
    UINT b = a;
    while (b > 0 && (p->cb[b - 1].set > c.set || (p->cb[b - 1].set == c.set && p->cb[b - 1].binding > c.binding))) p->cb[b] = p->cb[b - 1], --b;
    p->cb[b] = c;
  }
  for (UINT s = 0; s < nstages; ++s)
    for (UINT i = 0; i < spv_resource_count(stages[s].m); ++i) {
      SpvResource *r = spv_resource(stages[s].m, i);
      if (r->kind == SPV_RES_PUSH) add_cb_slot(p, SPV_RES_PUSH, 0xFFFFFFFFu, 0, (r->bytes + 15) / 16);
    }
  UINT total_vec4 = 0;
  for (UINT i = 0; i < p->ncb; ++i) {
    if (p->cb[i].vec4 == 0) p->cb[i].vec4 = 1;
    total_vec4 += p->cb[i].vec4;
  }
  if (total_vec4 * 16 > caps->max_push_constant_bytes) {
    *why = "the uniform data of the pipeline is more than the adapter's constant block (VkPhysicalDeviceLimits::maxPushConstantsSize)";
    return FALSE;
  }
  /* textures: the sampled images by (set, binding), each element a unit */
  p->nunit = 0;
  UINT keys[SP_MAX_RES_KEYS][3];
  UINT nkeys = 0;
  for (UINT s = 0; s < nstages; ++s)
    for (UINT i = 0; i < spv_resource_count(stages[s].m); ++i) {
      SpvResource *r = spv_resource(stages[s].m, i);
      if (r->kind != SPV_RES_IMAGE && r->kind != SPV_RES_COMBINED) continue;
      BOOL seen = FALSE;
      for (UINT k = 0; k < nkeys; ++k) seen |= keys[k][0] == r->set && keys[k][1] == r->binding;
      if (!seen && nkeys < SP_MAX_RES_KEYS) keys[nkeys][0] = r->set, keys[nkeys][1] = r->binding, keys[nkeys][2] = r->array_size, ++nkeys;
    }
  for (UINT a = 1; a < nkeys; ++a) {
    UINT c[3] = {keys[a][0], keys[a][1], keys[a][2]};
    UINT b = a;
    while (b > 0 && (keys[b - 1][0] > c[0] || (keys[b - 1][0] == c[0] && keys[b - 1][1] > c[1]))) memcpy(keys[b], keys[b - 1], sizeof c), --b;
    memcpy(keys[b], c, sizeof c);
  }
  for (UINT k = 0; k < nkeys; ++k)
    for (UINT e = 0; e < keys[k][2]; ++e) {
      if (p->nunit >= 8) break;
      TexUnit *u = &p->unit[p->nunit++];
      u->set = keys[k][0], u->binding = keys[k][1], u->element = e;
      u->sampler_set = u->sampler_binding = 0xFFFFFFFFu;
    }
  if (p->nunit > caps->max_texture_units) {
    *why = "the pipeline samples more textures than the adapter has texture units";
    return FALSE;
  }
  /* the numbers into the shaders' resources */
  for (UINT s = 0; s < nstages; ++s)
    for (UINT i = 0; i < spv_resource_count(stages[s].m); ++i) {
      SpvResource *r = spv_resource(stages[s].m, i);
      if (r->kind == SPV_RES_UNIFORM || r->kind == SPV_RES_PUSH) {
        UINT slot = 0;
        for (UINT k = 0; k < p->ncb; ++k) {
          if (p->cb[k].kind == r->kind && (r->kind == SPV_RES_PUSH || (p->cb[k].set == r->set && p->cb[k].binding == r->binding))) slot = k;
        }
        r->slot = slot;
      } else if (r->kind == SPV_RES_IMAGE || r->kind == SPV_RES_COMBINED) {
        for (UINT u = 0; u < p->nunit; ++u)
          if (p->unit[u].set == r->set && p->unit[u].binding == r->binding && p->unit[u].element == 0) r->slot = u;
      }
    }
  for (UINT s = 0; s < nstages; ++s)
    for (UINT i = 0; i < spv_resource_count(stages[s].m); ++i) {
      SpvResource *r = spv_resource(stages[s].m, i);
      if (r->kind == SPV_RES_IMAGE && r->sampler_slot < spv_resource_count(stages[s].m)) {
        SpvResource *sm = spv_resource(stages[s].m, r->sampler_slot);
        for (UINT u = 0; u < p->nunit; ++u)
          if (p->unit[u].set == r->set && p->unit[u].binding == r->binding) p->unit[u].sampler_set = sm->set, p->unit[u].sampler_binding = sm->binding;
      }
    }
  for (UINT s = 0; s < nstages; ++s)
    for (UINT k = 0; k < p->ncb; ++k) spv_declare_cb(stages[s].m, k, p->cb[k].vec4);
  return TRUE;
}

static void *spv_alloc(void *user, unsigned long long bytes) {
  (void)user;
  return vk_alloc((size_t)bytes);
}
static void spv_release(void *user, void *p) {
  (void)user;
  vk_free(p);
}

static UINT blend_factor(VkBlendFactor f, BOOL *bad) {
  switch (f) {
    case VK_BLEND_FACTOR_ZERO: return 0;
    case VK_BLEND_FACTOR_ONE: return 1;
    case VK_BLEND_FACTOR_SRC_COLOR: return 2;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return 3;
    case VK_BLEND_FACTOR_SRC_ALPHA: return 4;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return 5;
    case VK_BLEND_FACTOR_DST_COLOR: return 6;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return 7;
    case VK_BLEND_FACTOR_DST_ALPHA: return 8;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return 9;
    case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: return 10;
    default: *bad = TRUE; return 1;
  }
}

static VkResult create_one_pipeline(Device *d, const VkGraphicsPipelineCreateInfo *ci, Pipe **out) {
  const Caps *caps = &d->pd->caps;
  const VkPipelineShaderStageCreateInfo *vs_ci = NULL, *fs_ci = NULL;
  for (UINT i = 0; i < ci->stageCount; ++i) {
    if (ci->pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT) vs_ci = &ci->pStages[i];
    else if (ci->pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs_ci = &ci->pStages[i];
    else return VK_ERROR_FEATURE_NOT_PRESENT;   /* geometry, tessellation */
  }
  if (!vs_ci || !fs_ci || !ci->pVertexInputState || !ci->pInputAssemblyState || !ci->pRasterizationState) return VK_ERROR_INITIALIZATION_FAILED;
  if (!(caps->flags & CAP_PROGRAMMABLE_VERTEX) || !(caps->flags & CAP_PROGRAMMABLE_PIXEL)) return VK_ERROR_FEATURE_NOT_PRESENT;
  Pipe *p = vk_alloc(sizeof(Pipe));
  if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
  p->dev = d;
  p->layout = (PipeLayout *)ci->layout;
  p->subpass = ci->subpass;
  RenderPass *rp = (RenderPass *)ci->renderPass;
  p->color_targets = rp && ci->subpass < rp->nsub ? rp->sub[ci->subpass].ncolor : 1;
  if (p->color_targets > 1 && !(caps->features & FEAT_MULTIPLE_TARGETS)) {
    vk_free(p);
    return VK_ERROR_FEATURE_NOT_PRESENT;
  }
  VkResult result = VK_ERROR_INVALID_SHADER_NV;
  const char *why = NULL;
  Stage stages[2] = {{NULL, SPV_STAGE_VERTEX}, {NULL, SPV_STAGE_FRAGMENT}};
  const VkPipelineShaderStageCreateInfo *cis[2] = {vs_ci, fs_ci};
  SpvAllocator allocator = {spv_alloc, spv_release, NULL};
  for (UINT s = 0; s < 2; ++s) {
    const Shader *sh = (const Shader *)cis[s]->module;
    UINT ids[16], values[16], nspec = 0;
    if (cis[s]->pSpecializationInfo) {
      const VkSpecializationInfo *si = cis[s]->pSpecializationInfo;
      for (UINT k = 0; k < si->mapEntryCount && nspec < 16; ++k) {
        if (si->pMapEntries[k].size != 4 && si->pMapEntries[k].size != 1) continue;
        ids[nspec] = si->pMapEntries[k].constantID;
        values[nspec] = 0;
        memcpy(&values[nspec], (const BYTE *)si->pData + si->pMapEntries[k].offset, si->pMapEntries[k].size);
        ++nspec;
      }
    }
    stages[s].m = spv_open(sh->words, sh->nwords, &allocator);
    if (!stages[s].m || spv_reflect(stages[s].m, stages[s].stage, cis[s]->pName, ids, values, nspec)) {
      why = stages[s].m ? spv_error(stages[s].m) : "not a SPIR-V module";
      goto fail;
    }
  }
  /* vertex input: only the attributes the shader reads */
  const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
  UINT in_mask = spv_input_mask(stages[0].m);
  for (UINT i = 0; i < vi->vertexBindingDescriptionCount && p->nbindings < 8; ++i) {
    VtxBinding *b = &p->bindings[p->nbindings++];
    b->binding = vi->pVertexBindingDescriptions[i].binding, b->stride = vi->pVertexBindingDescriptions[i].stride;
    b->per_instance = vi->pVertexBindingDescriptions[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE;
  }
  UINT attr_flags = 0;
  for (UINT i = 0; i < vi->vertexAttributeDescriptionCount; ++i) {
    const VkVertexInputAttributeDescription *a = &vi->pVertexAttributeDescriptions[i];
    if (!((in_mask >> a->location) & 1)) continue;
    const VtxFormat *vf = vertex_format(a->format);
    if (!vf) {
      why = "a vertex attribute format the adapter cannot fetch";
      result = VK_ERROR_FORMAT_NOT_SUPPORTED;
      goto fail;
    }
    if (p->nattrs >= caps->max_vertex_attributes || p->nattrs >= 4) {
      why = "more vertex attributes than the adapter has";
      result = VK_ERROR_FEATURE_NOT_PRESENT;
      goto fail;
    }
    VtxAttr *va = &p->attrs[p->nattrs];
    va->location = a->location, va->binding = a->binding, va->offset = a->offset, va->format = a->format;
    p->out_offset[p->nattrs] = p->out_stride;
    p->out_format[p->nattrs] = vf->comps - 1;
    p->out_stride += 4 * vf->comps;
    for (UINT b = 0; b < p->nbindings; ++b)
      if (p->bindings[b].binding == a->binding && p->bindings[b].per_instance) attr_flags |= 1u << p->nattrs;
    ++p->nattrs;
  }
  if (p->out_stride > caps->max_vertex_stride && caps->max_vertex_stride) {
    why = "the vertex is wider than the adapter's vertex stride";
    result = VK_ERROR_FEATURE_NOT_PRESENT;
    goto fail;
  }
  /* varyings */
  UINT out_mask = spv_output_mask(stages[0].m), varyings = 0;
  for (UINT l = 0; l < 32; ++l)
    if ((out_mask >> l) & 1) varyings = l + 1;
  if (varyings > caps->max_varyings) {
    why = "more shader outputs than the adapter has varyings";
    result = VK_ERROR_FEATURE_NOT_PRESENT;
    goto fail;
  }
  if (!assign_resources(p, stages, 2, caps, &why)) {
    result = VK_ERROR_FEATURE_NOT_PRESENT;
    goto fail;
  }
  /* the shaders */
  KMT_HANDLE handles[2] = {0, 0};
  for (UINT s = 0; s < 2; ++s) {
    const BYTE *dxbc = NULL;
    UINT size = 0;
    if (spv_emit(stages[s].m, &dxbc, &size)) {
      why = spv_error(stages[s].m);
      goto fail;
    }
    if (!create_kernel_shader(d, dxbc, size, s, &handles[s])) {
      why = "the adapter does not take the translated shader";
      result = VK_ERROR_FEATURE_NOT_PRESENT;
      goto fail;
    }
  }
  p->vs = handles[0], p->fs = handles[1];
  /* fixed function state */
  const VkPipelineRasterizationStateCreateInfo *rs = ci->pRasterizationState;
  const VkPipelineDepthStencilStateCreateInfo *ds = ci->pDepthStencilState;
  const VkPipelineColorBlendStateCreateInfo *cb = ci->pColorBlendState;
  if (rs->cullMode == VK_CULL_MODE_FRONT_AND_BACK || rs->polygonMode != VK_POLYGON_MODE_FILL || rs->depthClampEnable) {
    why = "front-and-back culling, wireframe or depth clamping";
    result = VK_ERROR_FEATURE_NOT_PRESENT;
    goto fail;
  }
  p->topology = ci->pInputAssemblyState->topology;
  UINT words[96];
  UINT n = 0;
  n = put_words(words, n, 2);   /* CreatePipeline */
  n = put_words(words, n, p->vs);
  n = put_words(words, n, p->fs);
  n = put_words(words, n, p->out_stride);
  n = put_words(words, n, p->nattrs);
  n = put_words(words, n, varyings);
  n = put_words(words, n, p->nunit);
  n = put_words(words, n, rs->cullMode == VK_CULL_MODE_NONE ? 0 : rs->cullMode == VK_CULL_MODE_BACK_BIT ? 1 : 2);
  /* (the winding as seen on the screen, which is what both mean) */
  n = put_words(words, n, rs->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE ? 0 : 1);
  BOOL depth_test = ds && ds->depthTestEnable, depth_write = ds && ds->depthTestEnable && ds->depthWriteEnable;
  BOOL has_blend_state = (caps->features & FEAT_BLEND_STATE) != 0;
  const VkPipelineColorBlendAttachmentState *ba = cb && cb->attachmentCount ? &cb->pAttachments[0] : NULL;
  BOOL blend_on = ba && ba->blendEnable;
  BOOL plain_over = blend_on && ba->srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA && ba->dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA && ba->colorBlendOp == VK_BLEND_OP_ADD;
  if (blend_on && !has_blend_state && !plain_over) {
    why = "blending other than source over destination";
    result = VK_ERROR_FEATURE_NOT_PRESENT;
    goto fail;
  }
  n = put_words(words, n, (depth_test ? 1 : 0) | (depth_write ? 2 : 0) | (blend_on && !has_blend_state ? 4 : 0));
  for (UINT a = 0; a < 4; ++a) {
    n = put_words(words, n, a < p->nattrs ? p->attrs[a].location : 0);
    n = put_words(words, n, a < p->nattrs ? p->out_format[a] : 0);
    n = put_words(words, n, a < p->nattrs ? p->out_offset[a] : 0);
  }
  BOOL stencil = ds && ds->stencilTestEnable && (caps->flags & CAP_STENCIL);
  n = put_words(words, n, stencil ? 1 : 0);
  n = put_words(words, n, stencil ? ds->front.compareOp : 7);
  n = put_words(words, n, stencil ? ds->front.failOp : 0);
  n = put_words(words, n, stencil ? ds->front.depthFailOp : 0);
  n = put_words(words, n, stencil ? ds->front.passOp : 0);
  n = put_words(words, n, stencil ? ds->front.compareMask & 0xFF : 0xFF);
  n = put_words(words, n, stencil ? ds->front.writeMask & 0xFF : 0xFF);
  n = put_words(words, n, stencil ? ds->front.reference & 0xFF : 0);
  p->stencil_ref = stencil ? ds->front.reference & 0xFF : 0;
  if (has_blend_state || p->color_targets > 1 || attr_flags || (depth_test && ds->depthCompareOp != VK_COMPARE_OP_LESS_OR_EQUAL && ds->depthCompareOp != VK_COMPARE_OP_LESS)) {
    UINT compare = depth_test ? ds->depthCompareOp : VK_COMPARE_OP_LESS_OR_EQUAL;
    if (!has_blend_state && compare == VK_COMPARE_OP_LESS) compare = VK_COMPARE_OP_LESS_OR_EQUAL;   /* (the device only has "less or equal"; the difference is the pixels of equal depth) */
    BOOL bad = FALSE;
    n = put_words(words, n, compare);
    n = put_words(words, n, ba && has_blend_state ? 1 : 0);   /* custom */
    n = put_words(words, n, blend_on && has_blend_state ? 1 : 0);
    n = put_words(words, n, ba ? blend_factor(ba->srcColorBlendFactor, &bad) : 1);
    n = put_words(words, n, ba ? blend_factor(ba->dstColorBlendFactor, &bad) : 0);
    n = put_words(words, n, ba ? blend_factor(ba->srcAlphaBlendFactor, &bad) : 1);
    n = put_words(words, n, ba ? blend_factor(ba->dstAlphaBlendFactor, &bad) : 0);
    n = put_words(words, n, ba ? MIN(ba->colorBlendOp, 4u) : 0);
    n = put_words(words, n, ba ? MIN(ba->alphaBlendOp, 4u) : 0);
    n = put_words(words, n, ba ? ba->colorWriteMask & 15 : 15);
    n = put_words(words, n, attr_flags);
    if (bad && blend_on) {
      why = "blend constants";
      result = VK_ERROR_FEATURE_NOT_PRESENT;
      goto fail;
    }
    if (p->color_targets > 1) {
      n = put_words(words, n, p->color_targets);
      n = put_words(words, n, 0);   /* independent blending: no */
      for (UINT t = 0; t < 27; ++t) n = put_words(words, n, 0);
    }
  }
  if (!STATUS_OK(kmt_escape(d, words, n * 4))) {
    why = "the adapter refused the pipeline";
    result = VK_ERROR_FEATURE_NOT_PRESENT;
    goto fail;
  }
  p->pipe = words[0];
  if (!p->pipe) goto fail;
  if (ci->pViewportState && ci->pViewportState->pViewports && ci->pViewportState->viewportCount) p->viewport = ci->pViewportState->pViewports[0];
  if (ci->pViewportState && ci->pViewportState->pScissors && ci->pViewportState->scissorCount) p->scissor = ci->pViewportState->pScissors[0];
  if (ci->pDynamicState)
    for (UINT i = 0; i < ci->pDynamicState->dynamicStateCount; ++i) {
      if (ci->pDynamicState->pDynamicStates[i] == VK_DYNAMIC_STATE_VIEWPORT) p->dyn_viewport = TRUE;
      if (ci->pDynamicState->pDynamicStates[i] == VK_DYNAMIC_STATE_SCISSOR) p->dyn_scissor = TRUE;
    }
  for (UINT s = 0; s < 2; ++s) spv_close(stages[s].m);
  *out = p;
  return VK_SUCCESS;
fail:
  if (why) trace(why);
  for (UINT s = 0; s < 2; ++s)
    if (stages[s].m) spv_close(stages[s].m);
  vk_free(p);
  return result;
}

VKAPI VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *alloc, VkPipeline *out) {
  (void)cache, (void)alloc;
  VkResult first_error = VK_SUCCESS;
  for (UINT i = 0; i < count; ++i) {
    Pipe *p = NULL;
    VkResult r = create_one_pipeline((Device *)device, &infos[i], &p);
    out[i] = r == VK_SUCCESS ? (VkPipeline)p : VK_NULL_HANDLE;
    if (r != VK_SUCCESS && first_error == VK_SUCCESS) first_error = r;
  }
  return first_error;
}
VKAPI VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *alloc, VkPipeline *out) {
  (void)device, (void)cache, (void)infos, (void)alloc;
  for (UINT i = 0; i < count; ++i) out[i] = VK_NULL_HANDLE;
  return VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI void VKAPI_CALL vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *alloc) {
  Pipe *p = (Pipe *)pipeline;
  (void)alloc;
  if (!p) return;
  UINT req[2];
  if (p->pipe) req[0] = 4, req[1] = p->pipe, kmt_escape((Device *)device, req, 8);   /* DestroyPipeline */
  if (p->vs) req[0] = 3, req[1] = p->vs, kmt_escape((Device *)device, req, 8);       /* DestroyShader */
  if (p->fs) req[0] = 3, req[1] = p->fs, kmt_escape((Device *)device, req, 8);
  vk_free(p);
}
