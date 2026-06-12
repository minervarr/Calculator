[[vk::binding(0, 0)]] RWStructuredBuffer<float>   curves;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint>    tiles;
[[vk::binding(2, 0)]] RWTexture2D<float4>         outputImage;

struct PushConstants {
  uint screenWidth;
  uint screenHeight;
  uint curveCount;
  uint tileCountX;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> push;

float evaluateCurve(uint curveIndex, float px, float py)
{
  uint base = curveIndex * 16;
  uint type = uint(curves[base + 0]);
  float a   = curves[base + 1];
  float b   = curves[base + 2];
  float c   = curves[base + 3];
  float d   = curves[base + 4];
  float result = 1.0;

  if (type == 0u) {
    result = (a * px + b * py + c) / sqrt(a * a + b * b);
  };
  if (type == 1u) {
    float expected = a * px * px + b * px + c;
    result = py - expected;
  };
  if (type == 2u) {
    // Filled axis-aligned rect: a=cx, b=cy, c=halfW, d=halfH.
    float dx = abs(px - a) - c;
    float dy = abs(py - b) - d;
    result = max(dx, dy);
  };
  if (type == 3u) {
    // Line segment with caps: a=x0, b=y0, c=x1, d=y1.
    float2 p  = float2(px, py);
    float2 v0 = float2(a, b);
    float2 v1 = float2(c, d);
    float2 e  = v1 - v0;
    float  t  = saturate(dot(p - v0, e) / dot(e, e));
    float2 q  = v0 + e * t;
    result    = length(p - q);
  };

  return result;
}

[numthreads(16, 16 ,1)]
void main(uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
  uint px = groupId.x * 16 + localId.x;
  uint py = groupId.y * 16 + localId.y;

  if (px >= push.screenWidth || py >= push.screenHeight) return;

  float4 color = float4(0.0, 0.0, 0.0, 0.0);

  uint tileIndex = (py / 16) * push.tileCountX + (px/16);
  uint tileOffset = tileIndex * 17;
  uint count = tiles[tileOffset];

  for (uint i = 0; i < count && i < 16; i++) {
    uint curveIndex = tiles[tileOffset + 1 + i];
    uint base = curveIndex * 16;

    float4 curveColor = float4 (
      curves[base + 5],
      curves[base + 6],
      curves[base + 7],
      curves[base + 8]
    );
    float lineWidth = curves[base + 9];

    // DEBUG: recolour curves by index range so we can see what's reaching pixels.
    //   curveIndex 0     -> BG: original colour (dark gray)
    //   curveIndex 1     -> display panel: bright cyan
    //   curveIndex 2..5  -> display "0" segments: bright yellow
    //   curveIndex >= 6  -> buttons + labels: bright magenta
    if      (curveIndex == 1u) curveColor = float4(0.0, 1.0, 1.0, 1.0);
    else if (curveIndex >= 2u && curveIndex <= 5u) curveColor = float4(1.0, 1.0, 0.0, 1.0);
    else if (curveIndex >= 6u) curveColor = float4(1.0, 0.0, 1.0, 1.0);

    float dist = evaluateCurve(curveIndex, float(px), float(py));

    float alpha = (lineWidth > 0.0)
      ? (1.0 - smoothstep(lineWidth - 1.0, lineWidth + 1.0, abs(dist)))
      : (1.0 - smoothstep(-0.5, 0.5, dist));

    float effA = curveColor.a * alpha;
    float4 src = float4(curveColor.rgb * effA, effA);
    color = src + color * (1.0 - src.a);
  };

  outputImage[uint2(px, py)] = color;
}
