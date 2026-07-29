/*
 * Sensor Test Shader (Legacy GLSL)
 *
 * Visual diagnostic for gyroscope, accelerometer, and rest position uniforms.
 * Renders game as background with a HUD overlay showing live sensor bars.
 *
 * Gyroscope:     cyan bars, normalized by /10.0
 * Accelerometer: green bars, normalized by /15.0
 * Rest position: yellow marker lines on accel bars
 *
 * ACCEL_MODE 0 = raw accel values, 1 = delta from rest position
 */

#pragma parameter ACCEL_MODE "Accel Display Mode" 0.0 0.0 1.0 1.0

#if defined(VERTEX)

uniform mat4 MVPMatrix;
attribute vec4 VertexCoord;
attribute vec2 TexCoord;
varying vec2 vTexCoord;

void main()
{
   gl_Position = MVPMatrix * VertexCoord;
   vTexCoord   = TexCoord;
}

#elif defined(FRAGMENT)

#ifdef GL_ES
precision mediump float;
#endif

uniform sampler2D Texture;
uniform vec4 OutputSize;
uniform float GyroscopeX;
uniform float GyroscopeY;
uniform float GyroscopeZ;
uniform float AccelerometerX;
uniform float AccelerometerY;
uniform float AccelerometerZ;
uniform float AccelerometerRestX;
uniform float AccelerometerRestY;
uniform float AccelerometerRestZ;
uniform uint FrameCount;
uniform float ACCEL_MODE;

varying vec2 vTexCoord;

void main()
{
   vec3 game = texture2D(Texture, vTexCoord).rgb;

   /* HUD occupies bottom 30% of the screen */
   float hudTop = 0.30;
   if (vTexCoord.y < (1.0 - hudTop))
   {
      gl_FragColor = vec4(game, 1.0);
      return;
   }

   /* map fragment into HUD-local UV (0,0)=top-left of HUD */
   vec2 uv = vec2(vTexCoord.x, (vTexCoord.y - (1.0 - hudTop)) / hudTop);

   /* subtle pulse to show the shader is alive */
   float pulse = 0.92 + 0.08 * sin(float(FrameCount) * 0.05);

   /* semi-transparent dark backdrop */
   vec3  bg  = game * 0.25;
   float bgA = 0.75;

   /* normalised sensor values */
   float gx = GyroscopeX / 10.0;
   float gy = GyroscopeY / 10.0;
   float gz = GyroscopeZ / 10.0;

   float ax, ay, az;
   if (ACCEL_MODE > 0.5)
   {
      ax = (AccelerometerX - AccelerometerRestX) / 15.0;
      ay = (AccelerometerY - AccelerometerRestY) / 15.0;
      az = (AccelerometerZ - AccelerometerRestZ) / 15.0;
   }
   else
   {
      ax = AccelerometerX / 15.0;
      ay = AccelerometerY / 15.0;
      az = AccelerometerZ / 15.0;
   }

   float rx = AccelerometerRestX / 15.0;
   float ry = AccelerometerRestY / 15.0;
   float rz = AccelerometerRestZ / 15.0;

   /* composit bars */
   vec3  col   = bg;
   float alpha = bgA;

   vec3 cyan  = vec3(0.0, 0.85, 1.0) * pulse;
   vec3 green = vec3(0.1, 0.9,  0.3) * pulse;

   float totalRows = 6.0;
   float aspect    = OutputSize.x / OutputSize.y;

   /* iterate over 6 bar rows */
   for (int i = 0; i < 6; i++)
   {
      float rowH   = 1.0 / totalRows;
      float rowTop = 1.0 - float(i) * rowH;
      float rowBot = rowTop - rowH;

      /* vertical bounds with inner padding */
      float padV = rowH * 0.15;
      if (uv.y > rowTop - padV || uv.y < rowBot + padV) continue;

      /* horizontal: bar area spans 0.18 .. 0.92, center at 0.55 */
      float barL   = 0.18;
      float barR   = 0.92;
      float center = (barL + barR) * 0.5;
      float halfW  = (barR - barL) * 0.5;

      if (uv.x < barL || uv.x > barR) continue;

      float value;
      vec3  barColor;
      float restNorm = -100.0;

      if      (i == 0) { value = gx; barColor = cyan; }
      else if (i == 1) { value = gy; barColor = cyan; }
      else if (i == 2) { value = gz; barColor = cyan; }
      else if (i == 3) { value = ax; barColor = green; restNorm = (ACCEL_MODE > 0.5) ? 0.0 : rx; }
      else if (i == 4) { value = ay; barColor = green; restNorm = (ACCEL_MODE > 0.5) ? 0.0 : ry; }
      else             { value = az; barColor = green; restNorm = (ACCEL_MODE > 0.5) ? 0.0 : rz; }

      /* center line */
      if (abs(uv.x - center) < 0.002)
      {
         col = mix(col, vec3(0.6), 0.8);
         continue;
      }

      /* rest position marker (yellow line) */
      if (restNorm > -99.0)
      {
         float restX = center + clamp(restNorm, -1.0, 1.0) * halfW;
         if (abs(uv.x - restX) < 0.003)
         {
            col = vec3(1.0, 0.9, 0.2);
            continue;
         }
      }

      /* filled bar region */
      float valX = center + clamp(value, -1.0, 1.0) * halfW;
      bool filled = (value >= 0.0) ? (uv.x >= center && uv.x <= valX)
                                   : (uv.x <= center && uv.x >= valX);
      if (filled)
      {
         col = mix(col, barColor, 0.85);
      }
      else
      {
         col = mix(col, vec3(0.25), 0.3);
      }
   }

   /* section indicators: cyan tick for gyro rows (top half), green for accel (bottom half) */
   if (uv.x < 0.03 && uv.y > 0.5)
      col = mix(col, cyan, 0.5);
   if (uv.x < 0.03 && uv.y <= 0.5)
      col = mix(col, green, 0.5);

   /* axis labels: coloured dots — X=red, Y=green, Z=blue */
   float dotR = 0.008;
   for (int i = 0; i < 6; i++)
   {
      float rowCenter = 1.0 - (float(i) + 0.5) / 6.0;
      vec2 dotPos = vec2(0.08, rowCenter);
      float d = length((uv - dotPos) * vec2(aspect, 1.0));
      if (d < dotR)
      {
         int axis = i - (i / 3) * 3; /* i % 3 — avoid mod on old GLSL */
         if      (axis == 0) col = vec3(1.0, 0.3, 0.3);
         else if (axis == 1) col = vec3(0.3, 1.0, 0.3);
         else                col = vec3(0.4, 0.4, 1.0);
      }
   }

   gl_FragColor = vec4(col, 1.0);
}

#endif
