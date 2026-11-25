/* The Vulkan driver's string helpers and growing arrays: shared by the full driver (vk_base.inc) and the kernel's Venus-only build
 * (src/Graphics/Graphics3D/Venus/KernelVulkan.c). Needs vk_alloc, vk_free and memcpy (vk_os.h). */
/* ---- strings ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
static int streq(const char *a, const char *b) {
  while (*a && *a == *b) ++a, ++b;
  return *a == *b;
}
static size_t cstrlen(const char *s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}
static void copy_string(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
  dst[i] = 0;
}

static void *vk_realloc_array(void *old, size_t old_count, size_t new_count, size_t elem) {
  if (elem && new_count > SIZE_MAX / elem) return NULL;
  void *n = vk_alloc(new_count * elem);
  if (n && old) memcpy(n, old, old_count * elem);
  if (n) vk_free(old);
  return n;
}
/* Appends to a growing array (the item is copied); false when memory ran out. */
#define LIST_PUSH(list, count, cap, item)                                                       \
  (((count) < (cap) || ((list) = vk_realloc_array((list), (count), (cap) = (cap) ? (cap) * 2 : 8, sizeof((list)[0])))) ? ((list)[(count)++] = (item), 1) : 0)
