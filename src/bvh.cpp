#include "bvh.hh"

namespace psxsplash {

void BVHManager::initialize(const BVHNode *nodes, uint16_t nodeCount,
                            const TriangleRef *triangleRefs,
                            uint16_t triangleRefCount) {
  m_nodes = nodes;
  m_nodeCount = nodeCount;
  m_triangleRefs = triangleRefs;
  m_triangleRefCount = triangleRefCount;
}


int BVHManager::cullFrustum(const Frustum &frustum, TriangleRef *outRefs,
                            int maxRefs) const {
  if (!isLoaded() || m_nodeCount == 0)
    return 0;

  return traverseFrustum(0, frustum, outRefs, 0, maxRefs);
}

int BVHManager::traverseFrustum(int nodeIndex, const Frustum &frustum,
                                TriangleRef *outRefs, int currentCount,
                                int maxRefs) const {

  uint16_t stack[32];
  int top = 0;

  if (nodeIndex < 0 || nodeIndex >= m_nodeCount)
    return currentCount;

  stack[top++] = (uint16_t)nodeIndex;

  while (top > 0 && currentCount < maxRefs) {
    uint16_t idx = stack[--top];
    const BVHNode &node = m_nodes[idx];

    if (!frustum.testAABB(node))
      continue;

    if (node.isLeaf()) {
      int count = node.triangleCount;
      int available = maxRefs - currentCount;
      if (count > available)
        count = available;
      for (int i = 0; i < count; i++) {
        outRefs[currentCount + i] = m_triangleRefs[node.firstTriangle + i];
      }
      currentCount += count;
      continue;
    }

    // Push right first so left is processed first (stack is LIFO)
    if (node.rightChild != 0xFFFF && top < 32) {
      stack[top++] = node.rightChild;
    }
    if (node.leftChild != 0xFFFF && top < 32) {
      stack[top++] = node.leftChild;
    }
  }

  return currentCount;
}

int BVHManager::queryRegion(int32_t minX, int32_t minY, int32_t minZ,
                            int32_t maxX, int32_t maxY, int32_t maxZ,
                            TriangleRef *outRefs, int maxRefs) const {
  if (!isLoaded() || m_nodeCount == 0)
    return 0;

  return traverseRegion(0, minX, minY, minZ, maxX, maxY, maxZ, outRefs, 0,
                        maxRefs);
}

int BVHManager::traverseRegion(int nodeIndex, int32_t qMinX, int32_t qMinY,
                               int32_t qMinZ, int32_t qMaxX, int32_t qMaxY,
                               int32_t qMaxZ, TriangleRef *outRefs,
                               int currentCount, int maxRefs) const {
  // Iterative traversal using explicit stack.
  uint16_t stack[32];
  int top = 0;

  if (nodeIndex < 0 || nodeIndex >= m_nodeCount)
    return currentCount;

  stack[top++] = (uint16_t)nodeIndex;

  while (top > 0 && currentCount < maxRefs) {
    uint16_t idx = stack[--top];
    const BVHNode &node = m_nodes[idx];

    if (!aabbOverlap(node, qMinX, qMinY, qMinZ, qMaxX, qMaxY, qMaxZ))
      continue;

    if (node.isLeaf()) {
      int count = node.triangleCount;
      int available = maxRefs - currentCount;
      if (count > available)
        count = available;
      for (int i = 0; i < count; i++) {
        outRefs[currentCount + i] = m_triangleRefs[node.firstTriangle + i];
      }
      currentCount += count;
      continue;
    }

    if (node.rightChild != 0xFFFF && top < 32) {
      stack[top++] = node.rightChild;
    }
    if (node.leftChild != 0xFFFF && top < 32) {
      stack[top++] = node.leftChild;
    }
  }

  return currentCount;
}

bool BVHManager::aabbOverlap(const BVHNode &node, int32_t qMinX, int32_t qMinY,
                             int32_t qMinZ, int32_t qMaxX, int32_t qMaxY,
                             int32_t qMaxZ) {
  if (node.maxX < qMinX || node.minX > qMaxX)
    return false;
  if (node.maxY < qMinY || node.minY > qMaxY)
    return false;
  if (node.maxZ < qMinZ || node.minZ > qMaxZ)
    return false;
  return true;
}

} // namespace psxsplash
