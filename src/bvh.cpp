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
  if (nodeIndex < 0 || nodeIndex >= m_nodeCount)
    return currentCount;
  if (currentCount >= maxRefs)
    return currentCount;

  const BVHNode &node = m_nodes[nodeIndex];

  if (!frustum.testAABB(node)) {
    return currentCount;
  }

  if (node.isLeaf()) {
    int count = node.triangleCount;
    int available = maxRefs - currentCount;
    if (count > available)
      count = available;

    for (int i = 0; i < count; i++) {
      outRefs[currentCount + i] = m_triangleRefs[node.firstTriangle + i];
    }
    return currentCount + count;
  }

  if (node.leftChild != 0xFFFF) {
    currentCount = traverseFrustum(node.leftChild, frustum, outRefs,
                                   currentCount, maxRefs);
  }
  if (node.rightChild != 0xFFFF) {
    currentCount = traverseFrustum(node.rightChild, frustum, outRefs,
                                   currentCount, maxRefs);
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
  if (nodeIndex < 0 || nodeIndex >= m_nodeCount)
    return currentCount;
  if (currentCount >= maxRefs)
    return currentCount;

  const BVHNode &node = m_nodes[nodeIndex];

  if (!aabbOverlap(node, qMinX, qMinY, qMinZ, qMaxX, qMaxY, qMaxZ)) {
    return currentCount;
  }

  if (node.isLeaf()) {
    int count = node.triangleCount;
    int available = maxRefs - currentCount;
    if (count > available)
      count = available;

    for (int i = 0; i < count; i++) {
      outRefs[currentCount + i] = m_triangleRefs[node.firstTriangle + i];
    }
    return currentCount + count;
  }

  if (node.leftChild != 0xFFFF) {
    currentCount =
        traverseRegion(node.leftChild, qMinX, qMinY, qMinZ, qMaxX, qMaxY,
                       qMaxZ, outRefs, currentCount, maxRefs);
  }
  if (node.rightChild != 0xFFFF) {
    currentCount =
        traverseRegion(node.rightChild, qMinX, qMinY, qMinZ, qMaxX, qMaxY,
                       qMaxZ, outRefs, currentCount, maxRefs);
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
