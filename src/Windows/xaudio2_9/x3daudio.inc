/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* X3DAudio, exported from xaudio2_9.dll as in Windows (included by xaudio2_9.c): X3DAudioInitialize keeps the speaker layout and the speed of sound
 * in the 20-byte handle; X3DAudioCalculate works out, for each of the emitter's channels (spread around it at ChannelRadius by pChannelAzimuths), the
 * distance and the direction from the listener (azimuth in the listener's horizontal plane, from its front and top), and from those:
 *   - the matrix: the distance attenuation (the volume curve over distance / CurveDistanceScaler; without one, the inverse distance law, 1 within the
 *     scaler), the emitter's cone, and a constant-power pan between the two speakers either side of the direction (stereo: a sine-law pan); inside
 *     InnerRadius the sound spreads to all speakers; ZEROCENTER leaves the centre out, REDIRECT_TO_LFE feeds the LFE from the LFE curve;
 *   - the Doppler factor from both velocities along the line between them (DopplerScaler, the speed of sound);
 *   - the LPF and reverb levels from their curves (and the cone), the emitter angle, the distance and velocity components, and the inter-aural delay
 *     for a stereo layout. */

typedef struct {
  UINT32 mask;
  float speed;
  UINT32 speakers;      /* the count, LFE included */
  float reserved[2];
} X3dHandle;

static float x3_atan2(float y, float x) {
  /* (a rational approximation, good to about 0.005 rad) */
  const float pi = 3.14159265f;
  if (x == 0 && y == 0) return 0;
  const float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
  const float a = (ax < ay ? ax / ay : ay / ax);
  const float s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = pi / 2 - r;
  if (x < 0) r = pi - r;
  return y < 0 ? -r : r;
}
static float x3_dot(X3DAUDIO_VECTOR a, X3DAUDIO_VECTOR b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static X3DAUDIO_VECTOR x3_sub(X3DAUDIO_VECTOR a, X3DAUDIO_VECTOR b) { X3DAUDIO_VECTOR r = {a.x - b.x, a.y - b.y, a.z - b.z}; return r; }
static X3DAUDIO_VECTOR x3_cross(X3DAUDIO_VECTOR a, X3DAUDIO_VECTOR b) { X3DAUDIO_VECTOR r = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; return r; }
static float x3_length(X3DAUDIO_VECTOR a) { return (float)tcos_sqrt(x3_dot(a, a)); }
static X3DAUDIO_VECTOR x3_scale(X3DAUDIO_VECTOR a, float k) { X3DAUDIO_VECTOR r = {a.x * k, a.y * k, a.z * k}; return r; }
static X3DAUDIO_VECTOR x3_unit(X3DAUDIO_VECTOR a) { const float l = x3_length(a); return l > 1e-9f ? x3_scale(a, 1 / l) : a; }

/* A distance curve at `normalized` distance (piecewise linear; the ends hold). */
static float curve_at(const X3DAUDIO_DISTANCE_CURVE *c, float normalized, float fallback) {
  if (!c || !c->pPoints || c->PointCount == 0) return fallback;
  const X3DAUDIO_DISTANCE_CURVE_POINT *p = c->pPoints;
  if (normalized <= p[0].Distance) return p[0].DSPSetting;
  for (UINT32 k = 1; k < c->PointCount; ++k)
    if (normalized <= p[k].Distance) {
      const float span = p[k].Distance - p[k - 1].Distance;
      const float t = span > 1e-9f ? (normalized - p[k - 1].Distance) / span : 1;
      return p[k - 1].DSPSetting + (p[k].DSPSetting - p[k - 1].DSPSetting) * t;
    }
  return p[c->PointCount - 1].DSPSetting;
}

/* The speakers of a layout, in channel order: their azimuths (radians, clockwise from the front) and which is the LFE and the centre. */
static UINT32 speaker_layout(UINT32 mask, float *azimuth, int *lfe, int *center) {
  static const struct { UINT32 bit; float az; } kPositions[] = {
    {SPEAKER_FRONT_LEFT, -0.7853982f}, {SPEAKER_FRONT_RIGHT, 0.7853982f}, {SPEAKER_FRONT_CENTER, 0}, {SPEAKER_LOW_FREQUENCY, 0},
    {SPEAKER_BACK_LEFT, -2.3561945f}, {SPEAKER_BACK_RIGHT, 2.3561945f}, {SPEAKER_FRONT_LEFT_OF_CENTER, -0.3926991f},
    {SPEAKER_FRONT_RIGHT_OF_CENTER, 0.3926991f}, {SPEAKER_BACK_CENTER, 3.1415927f}, {SPEAKER_SIDE_LEFT, -1.5707964f}, {SPEAKER_SIDE_RIGHT, 1.5707964f},
  };
  UINT32 n = 0;
  *lfe = *center = -1;
  for (UINT32 k = 0; k < sizeof kPositions / sizeof kPositions[0]; ++k)
    if (mask & kPositions[k].bit) {
      if (kPositions[k].bit == SPEAKER_LOW_FREQUENCY) *lfe = (int)n;
      if (kPositions[k].bit == SPEAKER_FRONT_CENTER) *center = (int)n;
      azimuth[n++] = kPositions[k].az;
    }
  return n;
}

EXPORT HRESULT CDECL X3DAudioInitialize(UINT32 mask, float speed, X3DAUDIO_HANDLE handle) {
  if (!handle || !(speed >= 1.1754944e-38f)) return E_INVALIDARG;
  X3dHandle h;
  zero(&h, sizeof h);
  h.mask = mask ? mask : SPEAKER_STEREO;
  h.speed = speed;
  float az[32];
  int lfe, center;
  h.speakers = speaker_layout(h.mask, az, &lfe, &center);
  if (h.speakers == 0) return E_INVALIDARG;
  copy(handle, &h, sizeof h <= X3DAUDIO_HANDLE_BYTESIZE ? sizeof h : X3DAUDIO_HANDLE_BYTESIZE);
  return S_OK;
}

/* The gains of one direction across the speakers (the LFE and, with ZEROCENTER, the centre left out). */
static void pan_direction(float az, float spread, const float *speaker_az, UINT32 n, int lfe, int center, BOOL zero_center, float *gains) {
  for (UINT32 s = 0; s < n; ++s) gains[s] = 0;
  const float pi = 3.14159265f;
  int usable[32];
  UINT32 m = 0;
  for (UINT32 s = 0; s < n; ++s)
    if ((int)s != lfe && !(zero_center && (int)s == center)) usable[m++] = (int)s;
  if (m == 0) return;
  if (m == 1) {
    gains[usable[0]] = 1;
    return;
  }
  if (m == 2 && n <= 3) {
    /* stereo: a sine-law pan across the front (what is behind folds forward) */
    float p = (float)tcos_sin(az);
    const int left = speaker_az[usable[0]] < speaker_az[usable[1]] ? usable[0] : usable[1], right = left == usable[0] ? usable[1] : usable[0];
    gains[left] = (float)tcos_sqrt((1 - p) / 2);
    gains[right] = (float)tcos_sqrt((1 + p) / 2);
  } else {
    /* the two speakers either side of the direction, constant power between them */
    int a = -1, b = -1;
    float best_a = 10, best_b = 10;
    for (UINT32 k = 0; k < m; ++k) {
      float d = speaker_az[usable[k]] - az;
      while (d > pi) d -= 2 * pi;
      while (d < -pi) d += 2 * pi;
      if (d <= 0 && -d < best_a) { best_a = -d; a = usable[k]; }
      if (d >= 0 && d < best_b) { best_b = d; b = usable[k]; }
    }
    if (a < 0) a = b;
    if (b < 0) b = a;
    const float span = best_a + best_b;
    const float t = a == b || span < 1e-6f ? 0 : best_a / span;
    gains[a] += (float)tcos_cos(t * pi / 2);
    gains[b] += (float)tcos_sin(t * pi / 2);
  }
  /* inside the inner radius: towards every speaker alike */
  if (spread > 0) {
    const float even = (float)tcos_sqrt(1.0 / m);
    for (UINT32 k = 0; k < m; ++k) gains[usable[k]] = gains[usable[k]] * (1 - spread) + even * spread;
  }
}

EXPORT void CDECL X3DAudioCalculate(const X3DAUDIO_HANDLE handle, const X3DAUDIO_LISTENER *l, const X3DAUDIO_EMITTER *e, UINT32 flags, X3DAUDIO_DSP_SETTINGS *dsp) {
  if (!handle || !l || !e || !dsp) return;
  X3dHandle h;
  copy(&h, handle, sizeof h);
  float speaker_az[32];
  int lfe, center;
  const UINT32 n = speaker_layout(h.mask, speaker_az, &lfe, &center);
  const float scaler = e->CurveDistanceScaler > 1e-9f ? e->CurveDistanceScaler : 1;
  const X3DAUDIO_VECTOR front = x3_unit(l->OrientFront), top = x3_unit(l->OrientTop), right = x3_cross(top, front);
  const X3DAUDIO_VECTOR to_emitter = x3_sub(e->Position, l->Position);
  const float distance = x3_length(to_emitter);
  const float normalized = distance / scaler;
  const float volume = curve_at(e->pVolumeCurve, normalized, normalized > 1 ? 1 / normalized : 1);
  /* the cone: the angle between the emitter's front and the way to the listener */
  float cone_volume = 1, cone_lpf = 1, cone_reverb = 1, emitter_angle = 0;
  if (distance > 1e-9f) {
    const X3DAUDIO_VECTOR to_listener = x3_scale(to_emitter, -1 / distance);
    const float c = x3_dot(x3_unit(e->OrientFront), to_listener);
    emitter_angle = x3_atan2((float)tcos_sqrt(1 - (c > 1 ? 1 : c * c)), c);
    if (e->pCone) {
      const X3DAUDIO_CONE *k = e->pCone;
      const float angle = emitter_angle * 2;      /* (the cone's angles are whole widths) */
      float t = 0;
      if (angle <= k->InnerAngle) t = 0;
      else if (angle >= k->OuterAngle || k->OuterAngle <= k->InnerAngle) t = 1;
      else t = (angle - k->InnerAngle) / (k->OuterAngle - k->InnerAngle);
      cone_volume = k->InnerVolume + (k->OuterVolume - k->InnerVolume) * t;
      cone_lpf = k->InnerLPF + (k->OuterLPF - k->InnerLPF) * t;
      cone_reverb = k->InnerReverb + (k->OuterReverb - k->InnerReverb) * t;
    }
  }
  if (flags & X3DAUDIO_CALCULATE_MATRIX && dsp->pMatrixCoefficients) {
    const UINT32 src = dsp->SrcChannelCount, dst = dsp->DstChannelCount < n ? dsp->DstChannelCount : n;
    for (UINT32 k = 0; k < src * dsp->DstChannelCount; ++k) dsp->pMatrixCoefficients[k] = 0;
    for (UINT32 s = 0; s < src; ++s) {
      /* where this channel of the emitter is */
      X3DAUDIO_VECTOR at = e->Position;
      if (src > 1 && e->pChannelAzimuths) {
        const float a = e->pChannelAzimuths[s];
        const X3DAUDIO_VECTOR ef = x3_unit(e->OrientFront), et = x3_unit(e->OrientTop), er = x3_cross(et, ef);
        at.x += (ef.x * (float)tcos_cos(a) + er.x * (float)tcos_sin(a)) * e->ChannelRadius;
        at.y += (ef.y * (float)tcos_cos(a) + er.y * (float)tcos_sin(a)) * e->ChannelRadius;
        at.z += (ef.z * (float)tcos_cos(a) + er.z * (float)tcos_sin(a)) * e->ChannelRadius;
      }
      const X3DAUDIO_VECTOR rel = x3_sub(at, l->Position);
      const float az = x3_atan2(x3_dot(rel, right), x3_dot(rel, front));
      const float d = x3_length(rel);
      const float spread = e->InnerRadius > 0 && d < e->InnerRadius ? 1 - d / e->InnerRadius : 0;
      float gains[32];
      pan_direction(az, spread, speaker_az, n, lfe, center, (flags & X3DAUDIO_CALCULATE_ZEROCENTER) != 0, gains);
      for (UINT32 o = 0; o < dst; ++o) dsp->pMatrixCoefficients[o * src + s] = gains[o] * volume * cone_volume;
      if (lfe >= 0 && (UINT32)lfe < dst && (flags & X3DAUDIO_CALCULATE_REDIRECT_TO_LFE))
        dsp->pMatrixCoefficients[(UINT32)lfe * src + s] = curve_at(e->pLFECurve, normalized, volume) / (float)src;
    }
  }
  if ((flags & X3DAUDIO_CALCULATE_DELAY) && dsp->pDelayTimes) {
    /* the inter-aural time difference, up to 0.65 ms, for a stereo layout */
    const float az = x3_atan2(x3_dot(to_emitter, right), x3_dot(to_emitter, front));
    const float p = (float)tcos_sin(az);
    for (UINT32 o = 0; o < dsp->DstChannelCount; ++o) dsp->pDelayTimes[o] = 0;
    if (dsp->DstChannelCount >= 2 && n == 2) {
      dsp->pDelayTimes[0] = p > 0 ? p * 0.65f : 0;
      dsp->pDelayTimes[1] = p < 0 ? -p * 0.65f : 0;
    }
  }
  if (flags & X3DAUDIO_CALCULATE_LPF_DIRECT) dsp->LPFDirectCoefficient = curve_at(e->pLPFDirectCurve, normalized, 1) * cone_lpf;
  if (flags & X3DAUDIO_CALCULATE_LPF_REVERB) dsp->LPFReverbCoefficient = curve_at(e->pLPFReverbCurve, normalized, 1);
  if (flags & X3DAUDIO_CALCULATE_REVERB) dsp->ReverbLevel = curve_at(e->pReverbCurve, normalized, volume) * cone_reverb;
  /* velocities along the line from the emitter to the listener */
  float ev = 0, lv = 0;
  if (distance > 1e-9f) {
    const X3DAUDIO_VECTOR towards = x3_scale(to_emitter, -1 / distance);
    ev = x3_dot(e->Velocity, towards);
    lv = x3_dot(l->Velocity, towards);
  }
  dsp->EmitterVelocityComponent = ev;
  dsp->ListenerVelocityComponent = lv;
  if (flags & X3DAUDIO_CALCULATE_DOPPLER) {
    const float c = h.speed, k = e->DopplerScaler;
    float a = k * lv, b = k * ev;
    if (a > c * 0.9f) a = c * 0.9f;
    if (b > c * 0.9f) b = c * 0.9f;
    if (a < -c * 0.9f) a = -c * 0.9f;
    if (b < -c * 0.9f) b = -c * 0.9f;
    dsp->DopplerFactor = (c - a) / (c - b);
  } else {
    dsp->DopplerFactor = 1;
  }
  if (flags & X3DAUDIO_CALCULATE_EMITTER_ANGLE) dsp->EmitterToListenerAngle = emitter_angle;
  dsp->EmitterToListenerDistance = distance;
}
