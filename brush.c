#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <math.h>
#include "brush.h"
#include "helpers.h"
#include "brush_dab.h" 
;  // ; needed

#include "brush_settings.inc"

Brush * brush_create ()
{
  int i;
  Brush * b;
  b = g_new0 (Brush, 1);
  b->queue_draw_widget = NULL;
  for (i=0; brush_setting_infos[i].cname; i++) {
    brush_set_setting (b, i, brush_setting_infos[i].default_value);
  }
  return b;
}

Brush * brush_create_copy (Brush * old_b)
{
  Brush * b;
  b = g_new0 (Brush, 1);
  memcpy (b, old_b, sizeof(Brush));
  return b;
}

void brush_free (Brush * b)
{
  g_free (b);
}

void brush_reset (Brush * b)
{
  b->time = 0;
}

void brush_mutate (Brush * b)
{
#if 0
  int i;
  for (i=0; i<F_WEIGHTS; i++) {
    /*
    if (g_random_int_range(0, 10) == 0) {
      b->variations[i] *= g_random_double_range(0.5, 1.0/0.5);
      }*/
    b->weights[i] += g_random_double_range(-b->variations[i], b->variations[i]);
  }
#endif
}

float exp_decay (float T_const, float t)
{
  // FIXME: think about whether the argument make mathematical sense
  if (T_const <= 0.001) {
    return 1.0;
  } else {
    return 1.0-exp(- t / T_const);
  }
}

// high-level part of before each dab
void brush_prepare_and_draw_dab (Brush * b, Surface * s)
{
  float x, y, radius_log, radius, opaque;
  float speed;

  g_assert (b->pressure >= 0 && b->pressure <= 1);

  { // slow position 2
    float fac = exp_decay (b->position_T2, 0.4);
    b->x_slow += (b->x - b->x_slow) * fac;
    b->y_slow += (b->y - b->y_slow) * fac;
    x = b->x_slow;
    y = b->y_slow;
  }

  //x = b->x; y = b->y;
  radius_log = b->radius_logarithmic;
  opaque = b->opaque;
  
  speed = sqrt(sqr(b->dx) + sqr(b->dy))/b->dtime;

  { // slow speed
    float fac = exp_decay (b->obs__speed_slowness * 0.01, 0.1 * b->dtime);
    b->dx_slow += (b->dx - b->dx_slow) * fac;
    b->dy_slow += (b->dy - b->dy_slow) * fac;

    fac = exp_decay (b->obs__speedabs_slowness, 0.1 * b->dtime);
    b->obs__speedabs_slow += (speed - b->obs__speedabs_slow) * fac;

    fac = exp_decay (b->rbs__speedabs_slowness * 0.001, 0.1 * b->dtime);
    b->rbs__speedabs_slow += (speed - b->rbs__speedabs_slow) * fac;
  }

  // TODO: think about it: is this setting enough?
  //opaque *= (b->opaque_by_pressure * b->pressure + (1-b->opaque_by_pressure));
  opaque *= b->opaque_by_pressure * b->pressure;
  if (opaque >= 1.0) opaque = 1.0;
  //b->radius = 2.0 + sqrt(sqrt(speed));
  radius_log += b->pressure * b->radius_by_pressure;

  if (b->radius_by_random) {
    radius_log += (g_random_double () - 0.5) * b->radius_by_random;
  }

  if (b->radius_by_speed) {
    radius_log += 0.001 * b->rbs__speedabs_slow * b->radius_by_speed;
  }

  if (b->offset_by_random) {
    x += gauss_noise () * b->offset_by_random;
    y += gauss_noise () * b->offset_by_random;
  }

  if (b->offset_by_speed) {
    //x += b->dx_slow * b->offset_by_speed; // * radius?
    //y += b->dy_slow * b->offset_by_speed;
    x += b->dx_slow * 0.01*b->obs__speedabs_slow * b->offset_by_speed; // * radius?
    y += b->dy_slow * 0.01*b->obs__speedabs_slow * b->offset_by_speed;
  }

#if 0
  i = 0;
  b->opaque  = 0;
  b->opaque += b->weights[i++] * b->pressure;
  b->opaque += b->weights[i++] * speed;
  b->opaque += b->weights[i++] * 1.0;
  b->opaque += b->weights[i++] * noise;
  b->radius  = 0;
  b->radius += b->weights[i++] * b->pressure;
  b->radius += b->weights[i++] * speed;
  b->radius += b->weights[i++] * 1.0;
  b->radius += b->weights[i++] * noise;
  g_assert (i == F_WEIGHTS);
#endif

  radius = expf(radius_log);

  g_assert(radius > 0);
  if (radius > 200) radius = 200;
  g_assert(opaque >= 0);
  g_assert(opaque <= 1);

  // used for interpolation later
  b->actual_radius = radius;
  
  draw_brush_dab (s, b->queue_draw_widget,
                  x, y, radius, opaque, b->hardness, b->color, b->saturation_slowdown);
}

float brush_count_dabs_to (Brush * b, float x, float y, float pressure, float time)
{
  float dx, dy, dt;
  float res1, res2, res3;
  float dist;
  if (b->actual_radius == 0) b->actual_radius = expf(b->radius_logarithmic);
  if (b->actual_radius < 0.5) b->actual_radius = 0.5;
  if (b->actual_radius > 500.0) b->actual_radius = 500.0;
  dx = x - b->x;
  dy = y - b->y;
  //dp = pressure - b->pressure; // Not useful?
  dt = time - b->time;

  dist = sqrtf (dx*dx + dy*dy);
  res1 = dist / b->actual_radius * b->dabs_per_actual_radius;
  res2 = dist / expf(b->radius_logarithmic) * b->dabs_per_basic_radius;
  res3 = dt * b->dabs_per_second;
  return res1 + res2 + res3;
}

void brush_stroke_to (Brush * b, Surface * s, float x, float y, float pressure, float time)
{
  float dist;
  if (time <= b->time) return;

  if (b->time == 0 || time - b->time > 5) {
    // reset
    b->x = x;
    b->y = y;
    b->pressure = pressure;
    b->time = time;

    b->last_time = b->time;
    b->x_slow = b->x;
    b->y_slow = b->y;
    b->dx_slow = 0.0;
    b->dy_slow = 0.0;
    return;
  }

  { // calculate the actual "virtual" cursor position
    float fac = exp_decay (b->position_T, 100.0*(time - b->last_time));
    x = b->x + (x - b->x) * fac;
    y = b->y + (y - b->y) * fac;
  }
  // draw many (or zero) dabs to the next position
  dist = brush_count_dabs_to (b, x, y, pressure, time);
  //g_print("dist = %f\n", dist);
  // Not going to recalculate dist each step.

  while (dist >= 1.0) {
    { // linear interpolation
      // Inside the loop because outside it produces numerical errors
      // resulting in b->pressure being small negative and such effects.
      float step;
      step = 1 / dist;
      b->dx        = step * (x - b->x);
      b->dy        = step * (y - b->y);
      b->dpressure = step * (pressure - b->pressure);
      b->dtime     = step * (time - b->time);
    }
    
    b->x        += b->dx;
    b->y        += b->dy;
    b->pressure += b->dpressure;
    b->time     += b->dtime;

    dist -= 1.0;
    
    brush_prepare_and_draw_dab (b, s);
  }

  // not equal to b_time now unless dist == 0
  b->last_time = time;
}

