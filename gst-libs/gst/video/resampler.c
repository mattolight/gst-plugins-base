/* GStreamer
 * Copyright (C) <2014> Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "resampler.h"

typedef struct _ResamplerParams ResamplerParams;

struct _ResamplerParams
{
  GstResamplerMethod method;
  GstResamplerFlags flags;

  gdouble shift;

    gdouble (*get_tap) (ResamplerParams * params, gint l, gint xi, gdouble x);

  /* used by lanczos */
  gdouble ex, fx, dx;
  /* extra params */
  gdouble envelope;
  gdouble sharpness;
  gdouble sharpen;

  GstResampler *resampler;
};

static double
sinc (double x)
{
  if (x == 0)
    return 1;

  return sin (G_PI * x) / (G_PI * x);
}

static double
envelope (double x)
{
  if (x <= -1 || x >= 1)
    return 0;
  return sinc (x);
}

static gdouble
get_nearest_tap (ResamplerParams * params, gint l, gint xi, gdouble x)
{
  return 1.0;
}

static gdouble
get_linear_tap (ResamplerParams * params, gint l, gint xi, gdouble x)
{
  gdouble n_taps;
  gdouble res, a;
  gint xl = xi + l;

  n_taps = (params->resampler->max_taps + 1) / 2;

  a = fabs (x - xl);

  if (a < n_taps)
    res = (n_taps - a) / (gdouble) n_taps;
  else
    res = 0.0;

  return res;
}

static gdouble
bicubic (gdouble s)
{
  s = fabs (s);

  if (s <= 1.0)
    return 3.0 * (s * s * s) / 2.0 - 5.0 * (s * s) / 2.0 + 1.0;
  else if (s <= 2.0)
    return -1.0 * (s * s * s) / 2.0 + 5.0 * (s * s) / 2.0 - 4.0 * s + 2.0;
  else
    return 0.0;
}

static gdouble
get_bicubic_tap (ResamplerParams * params, gint l, gint xi, gdouble x)
{
  gdouble a, res;

  a = x - (xi + 1);

  if (l == 0)
    res = bicubic (1.0 + a);
  else if (l == 1)
    res = bicubic (a);
  else if (l == 2)
    res = bicubic (1.0 - a);
  else
    res = bicubic (2.0 - a);

  return res;
}

static gdouble
get_sinc_tap (ResamplerParams * params, gint l, gint xi, gdouble x)
{
  gint xl = xi + l;
  return sinc (x - xl);
}

static gdouble
get_lanczos_tap (ResamplerParams * params, gint l, gint xi, gdouble x)
{
  gint xl = xi + l;
  gdouble env = envelope ((x - xl) * params->ex);
  return (sinc ((x - xl) * params->fx) - params->sharpen) * env;
}

static void
resampler_calculate_taps (ResamplerParams * params)
{
  GstResampler *resampler = params->resampler;
  gint j;
  guint *offset, *n_taps, *phase;
  gint tap_offs;
  gint max_taps;
  gint in_size, out_size;
  gdouble shift;
  gdouble corr;

  in_size = resampler->in_size;
  out_size = resampler->out_size;

  max_taps = resampler->max_taps;
  tap_offs = (max_taps - 1) / 2;
  corr = (max_taps == 1 ? 0.5 : 0.0);

  shift = params->shift;

  resampler->taps = g_malloc (sizeof (gdouble) * max_taps * out_size);
  n_taps = resampler->n_taps = g_malloc (sizeof (guint) * out_size);
  offset = resampler->offset = g_malloc (sizeof (guint) * out_size);
  phase = resampler->phase = g_malloc (sizeof (guint) * out_size);

  for (j = 0; j < out_size; j++) {
    gdouble x;
    gint xi;
    gint l;
    gdouble weight;
    gdouble *taps;

    /* x is the source pixel to use, can be fractional */
    x = shift + (in_size * j) / (gdouble) out_size;
    x = CLAMP (x, 0, in_size - 1);
    /* this is the first source pixel to use */
    xi = floor (x + corr) - tap_offs;

    offset[j] = xi;
    phase[j] = j;
    n_taps[j] = max_taps;
    weight = 0;
    taps = resampler->taps + j * max_taps;

    for (l = 0; l < max_taps; l++) {
      taps[l] = params->get_tap (params, l, xi, x);
      weight += taps[l];
    }

    for (l = 0; l < max_taps; l++)
      taps[l] /= weight;

    if (xi < 0) {
      gint sh = -xi;

      for (l = 0; l < sh; l++) {
        taps[sh] += taps[l];
      }
      for (l = 0; l < max_taps - sh; l++) {
        taps[l] = taps[sh + l];
      }
      for (; l < max_taps; l++) {
        taps[l] = 0;
      }
      offset[j] += sh;
    }
    if (xi > in_size - max_taps) {
      gint sh = xi - (in_size - max_taps);

      for (l = 0; l < sh; l++) {
        taps[max_taps - sh - 1] += taps[max_taps - sh + l];
      }
      for (l = 0; l < max_taps - sh; l++) {
        taps[max_taps - 1 - l] = taps[max_taps - 1 - sh - l];
      }
      for (l = 0; l < sh; l++) {
        taps[l] = 0;
      }
      offset[j] -= sh;
    }
  }
}

/**
 * gst_resampler_new:
 * @resampler: a #GstResampler
 * @method: a #GstResamplerMethod
 * @flags: #GstResamplerFlags
 * @n_phases: number of phases to use
 * @n_taps: number of taps to use
 * @in_size: number of source elements
 * @out_size: number of destination elements
 * @options: extra options
 *
 * Make a new resampler. @in_size source elements will
 * be resampled to @out_size destination elements.
 *
 * @n_taps specifies the amount of elements to use from the source for one output
 * element. If n_taps is 0, this function chooses a good value automatically based
 * on the @method and @in_size/@out_size.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.6
 */
gboolean
gst_resampler_init (GstResampler * resampler,
    GstResamplerMethod method, GstResamplerFlags flags,
    guint n_phases, guint n_taps, gdouble shift, guint in_size, guint out_size,
    GstStructure * options)
{
  ResamplerParams params;

  g_return_val_if_fail (in_size != 0, FALSE);
  g_return_val_if_fail (out_size != 0, FALSE);
  g_return_val_if_fail (n_phases == out_size, FALSE);

  resampler->in_size = in_size;
  resampler->out_size = out_size;
  resampler->max_taps = n_taps;
  resampler->n_phases = n_phases;

  params.method = method;
  params.flags = flags;
  params.shift = shift;
  params.resampler = resampler;

  GST_DEBUG ("%d %u  %u->%u", method, n_taps, in_size, out_size);

  switch (method) {
    case GST_RESAMPLER_METHOD_NEAREST:
      params.get_tap = get_nearest_tap;
      if (n_taps == 0)
        n_taps = 1;
      break;
    case GST_RESAMPLER_METHOD_LINEAR:
      params.get_tap = get_linear_tap;
      if (n_taps == 0)
        n_taps = 2;
      break;
    case GST_RESAMPLER_METHOD_BICUBIC:
      n_taps = 4;
      params.get_tap = get_bicubic_tap;
      break;
    case GST_RESAMPLER_METHOD_SINC:
      params.get_tap = get_sinc_tap;
      if (n_taps == 0)
        n_taps = 4;
      break;
    case GST_RESAMPLER_METHOD_LANCZOS:
    {
      gdouble resample_inc = in_size / (gdouble) out_size;

      params.envelope = 2.0;
      params.sharpness = 1.0;
      params.sharpen = 0.0;

      if (resample_inc > 1.0) {
        params.fx = (1.0 / resample_inc) * params.sharpness;
      } else {
        params.fx = (1.0) * params.sharpness;
      }
      params.ex = params.fx / params.envelope;
      params.dx = ceil (params.envelope / params.fx);

      if (n_taps == 0)
        n_taps = 2 * params.dx;
      params.get_tap = get_lanczos_tap;
      break;
    }
    default:
      break;
  }
  resampler_calculate_taps (&params);

#if 0
  {
    gint i, max_taps;

    max_taps = resampler->max_taps;

    for (i = 0; i < out_size; i++) {
      gint j, o, n_taps;
      gdouble sum;

      o = resampler->offsets[i];
      n_taps = resampler->n_taps[i];

      printf ("%u: \t%d  ", i, o);
      sum = 0;
      for (j = 0; j < n_taps; j++) {
        gdouble tap;
        tap = resampler->taps[i * max_taps + j];
        printf ("\t%f ", tap);
        sum += tap;
      }
      printf ("\t: sum %f\n", sum);
    }
  }
#endif
  return TRUE;
}

/**
 * gst_resampler_clear:
 * @resampler: a #GstResampler
 *
 * Clear a previously initialized #GstResampler @resampler.
 *
 * Since: 1.6
 */
void
gst_resampler_clear (GstResampler * resampler)
{
  g_return_if_fail (resampler != NULL);

  g_free (resampler->phase);
  g_free (resampler->offset);
  g_free (resampler->n_taps);
  g_free (resampler->taps);
}