// This is the first computer pass.
[[vk::binding(0, 0)]] RWStructuredBuffer<float> curves;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> tiles;
struct PushConstants {
  uint screenWidth;
  uint screenHeight;
  uint curveCount;
  uint tileCountX;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> push;

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
  uint curveIndex = dispatchId.x;

  if (curveIndex >= push.curveCount) return;

  uint base = curveIndex * 16;
  float minX = curves[base + 10];
  float minY = curves[base + 11];
  float maxX = curves[base + 12];
  float maxY = curves[base + 13];

  uint tileMinX = uint(max(0.0, minX)) / 16;
  uint tileMinY = uint(max(0.0, minY)) / 16;
  uint tileMaxX = uint(min(float(push.screenWidth - 1), maxX)) / 16;
  uint tileMaxY = uint(min(float(push.screenHeight - 1), maxY)) / 16;

  for (uint ty = tileMinY; ty <= tileMaxY; ty++) {
    for (uint tx = tileMinX; tx <= tileMaxX; tx++) {
      
      uint tileIndex = ty * push.tileCountX + tx;
      uint tileOffset = tileIndex * 17;

      uint slot;
      InterlockedAdd(tiles[tileOffset], 1, slot);

      if (slot < 16) {
        tiles[tileOffset + 1 + slot] = curveIndex;
      };
    };
  };
}
