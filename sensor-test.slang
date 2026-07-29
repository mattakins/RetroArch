#version 450

/*
 * Sensor Test Shader (Slang)
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

layout(std140, set = 0, binding = 0) uniform UBO
{
   mat4 MVP;
   vec4 SourceSize;
   vec4 OutputSize;
   float GyroscopeX;
   float GyroscopeY;
   float GyroscopeZ;
   float AccelerometerX;
   float AccelerometerY;
   float AccelerometerZ;
   float AccelerometerRestX;
   float AccelerometerRestY;
   float AccelerometerRestZ;
   uint FrameCount;
};

layout(push_constant) uniform Push
{
   float ACCEL_MODE;
} params;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
   gl_Position = MVP * Position;
   vTexCoord   = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

/* ── helper: draw a single horizontal bar ────────────────────────── */
/*   uv        – fragment position in HUD-local coords (0..1)        */
/*   row       – which bar slot (0-5)                                 */
/*   value     – normalized bar value (-1..+1 range after scaling)    */
/*   barColor  – base bar colour                                      */
/*   restNorm  – normalized rest marker position (negative = no mark) */
float barMask(vec2 uv, int row, float value, out vec3 color, vec3 barColor, float restNorm)
{
   float totalRows  = 6.0;
   float rowH       = 1.0 / totalRows;
   float rowTop     = 1.0 - float(row) * rowH;
   float rowBot     = rowTop - rowH;

   /* vertical bounds with inner padding */
   float padV = rowH * 0.15;
   if (uv.y > rowTop - padV || uv.y < rowBot + padV) return 0.0;

   /* horizontal: bar area spans 0.18 .. 0.92, center at 0.55 */
   float barL   = 0.18;
   float barR   = 0.92;
   float center = (barL + barR) * 0.5;
   float halfW  = (barR - barL) * 0.5;

   if (uv.x < barL || uv.x > barR) return 0.0;

   /* center line */
   if (abs(uv.x - center) < 0.002)
   {
      color = vec3(0.6);
      return 0.8;
   }

   /* rest position marker (yellow line) */
   if (restNorm > -99.0)
   {
      float restX = center + clamp(restNorm, -1.0, 1.0) * halfW;
      if (abs(uv.x - restX) < 0.003)
      {
         color = vec3(1.0, 0.9, 0.2);
         return 1.0;
      }
   }

   /* filled bar region */
   float valX = center + clamp(value, -1.0, 1.0) * halfW;
   bool filled = (value >= 0.0) ? (uv.x >= center && uv.x <= valX)
                                : (uv.x <= center && uv.x >= valX);
   if (filled)
   {
      color = barColor;
      return 0.85;
   }

   /* unfilled track */
   color = vec3(0.25);
   return 0.3;
}

void main()
{
   vec3 game = texture(Source, vTexCoord).rgb;

   /* HUD occupies bottom 30% of the screen */
   float hudTop = 0.30;
   if (vTexCoord.y < (1.0 - hudTop))
   {
      FragColor = vec4(game, 1.0);
      return;
   }

   /* map fragment into HUD-local UV (0,0)=top-left of HUD */
   vec2 uv = vec2(vTexCoord.x, (vTexCoord.y - (1.0 - hudTop)) / hudTop);

   /* subtle pulse to show the shader is alive */
   float pulse = 0.92 + 0.08 * sin(float(FrameCount) * 0.05);

   /* semi-transparent dark backdrop */
   vec3  bg    = game * 0.25;
   float bgA   = 0.75;

   /* normalised sensor values */
   float gx = GyroscopeX     / 10.0;
   float gy = GyroscopeY     / 10.0;
   float gz = GyroscopeZ     / 10.0;

   float ax, ay, az;
   if (params.ACCEL_MODE > 0.5)
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

   /* rows: 0=GyroX, 1=GyroY, 2=GyroZ, 3=AccelX, 4=AccelY, 5=AccelZ */
   vec3 barCol;
   float m;

   m = barMask(uv, 0, gx, barCol, cyan,  -100.0); if (m > 0.0) { col = mix(col, barCol, m); alpha = max(alpha, m); }
   m = barMask(uv, 1, gy, barCol, cyan,  -100.0); if (m > 0.0) { col = mix(col, barCol, m); alpha = max(alpha, m); }
   m = barMask(uv, 2, gz, barCol, cyan,  -100.0); if (m > 0.0) { col = mix(col, barCol, m); alpha = max(alpha, m); }
   m = barMask(uv, 3, ax, barCol, green, (params.ACCEL_MODE > 0.5) ? 0.0 : rx); if (m > 0.0) { col = mix(col, barCol, m); alpha = max(alpha, m); }
   m = barMask(uv, 4, ay, barCol, green, (params.ACCEL_MODE > 0.5) ? 0.0 : ry); if (m > 0.0) { col = mix(col, barCol, m); alpha = max(alpha, m); }
   m = barMask(uv, 5, az, barCol, green, (params.ACCEL_MODE > 0.5) ? 0.0 : rz); if (m > 0.0) { col = mix(col, barCol, m); alpha = max(alpha, m); }

   /* labels — approximate with coloured vertical ticks in left margin */
   /* Gyro section indicator: cyan tick at left edge for rows 0-2 */
   if (uv.x < 0.03 && uv.y > (1.0 - 3.0/6.0))
      col = mix(col, cyan, 0.5);
   /* Accel section indicator: green tick at left edge for rows 3-5 */
   if (uv.x < 0.03 && uv.y <= (1.0 - 3.0/6.0))
      col = mix(col, green, 0.5);

   /* axis labels via small coloured dots: X=red, Y=green, Z=blue at x=0.08 */
   float dotR = 0.008;
   for (int i = 0; i < 6; i++)
   {
      float rowCenter = 1.0 - (float(i) + 0.5) / 6.0;
      vec2 dotPos = vec2(0.08, rowCenter);
      float d = length((uv - dotPos) * vec2(OutputSize.x / OutputSize.y, 1.0));
      if (d < dotR)
      {
         int axis = i % 3; /* 0=X, 1=Y, 2=Z */
         vec3 axisCol = (axis == 0) ? vec3(1.0, 0.3, 0.3)
                      : (axis == 1) ? vec3(0.3, 1.0, 0.3)
                                    : vec3(0.4, 0.4, 1.0);
         col = axisCol;
      }
   }

   /* numeric value labels — encode as bar brightness at the right edge */
   /* (text rendering is impractical in shader, so we use the bar fill itself) */

   FragColor = vec4(col, 1.0);
}
