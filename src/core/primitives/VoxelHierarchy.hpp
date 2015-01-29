#ifndef VOXELHIERARCHY_HPP_
#define VOXELHIERARCHY_HPP_

#include "math/Vec.hpp"
#include "math/Ray.hpp"

#include <memory>
#include <limits>
#include <array>

namespace Tungsten {

template<int SizePower, int NumLevels, typename ElementType>
class VoxelHierarchy
{
    static CONSTEXPR int BrickSize = 1 << SizePower;

    struct Cubelet
    {
        std::array<ElementType, (1 << 3*SizePower)> _data;

        ElementType &at(int x, int y, int z)
        {
            return _data[x + (y << SizePower) + (z << 2*SizePower)];
        }
        ElementType at(int x, int y, int z) const
        {
            return _data[x + (y << SizePower) + (z << 2*SizePower)];
        }
    };

    Vec3f _offset;
    std::array<std::unique_ptr<Cubelet[]>, NumLevels> _grids;

    void buildHierarchy(int level, const ElementType *data, ElementType *parent)
    {
        const int size = 1 << ((NumLevels - level)*SizePower);
        const int parentSize = size >> SizePower;

        auto brickContainsVoxels = [&](int bx, int by, int bz) {
            for (int z = bz*BrickSize; z < (bz + 1)*BrickSize; ++z)
                for (int y = by*BrickSize; y < (by + 1)*BrickSize; ++y)
                    for (int x = bx*BrickSize; x < (bx + 1)*BrickSize; ++x)
                        if (data[x + size*y + size*size*z])
                            return true;
            return false;
        };
        auto copyContainsVoxels = [&](int bx, int by, int bz, Cubelet &dst) {
            bool contains = false;
            int base = (bx + size*by + size*size*bz)*BrickSize;

            for (int z = 0; z < BrickSize; ++z)
                for (int y = 0; y < BrickSize; ++y)
                    for (int x = 0; x < BrickSize; ++x)
                        if ((dst.at(x, y, z) = data[base + x + size*y + size*size*z]))
                            contains = true;
            return contains;
        };

        int nonZeroCount = 0;
        for (int z = 0; z < parentSize; ++z)
            for (int y = 0; y < parentSize; ++y)
                for (int x = 0; x < parentSize; ++x)
                    if (brickContainsVoxels(x, y, z))
                        nonZeroCount++;

        _grids[level].reset(new Cubelet[nonZeroCount]);

        std::memset(parent, 0, sizeof(ElementType)*parentSize*parentSize*parentSize);

        int count = 0;
        for (int z = 0; z < parentSize && count < nonZeroCount; ++z) {
            for (int y = 0; y < parentSize && count < nonZeroCount; ++y) {
                for (int x = 0; x < parentSize && count < nonZeroCount; ++x) {
                    if (copyContainsVoxels(x, y, z, _grids[level][count])) {
                        parent[x + parentSize*y + parentSize*parentSize*z] = count + 1;
                        count++;
                    }
                }
            }
        }
    }

    template<int Level, typename Intersect>
    inline bool dda(const Cubelet &cube, const Vec3f &o, const Vec3f &dir, float tMin, float tMax, const Vec3f &dT,
            Vec3i corner, Intersect intersect) const
    {
        Vec3f p  = o + dir*tMin;
        Vec3i iP = Vec3i(p) >> (Level*SizePower);

        Vec3f nextT;
        Vec3i iStep;
        for (int i = 0; i < 3; ++i) {
            if (dir[i] > 0.0f) {
                iP   [i] = corner[i] + max(iP[i] - corner[i], 0);
                nextT[i] = tMin + (float((iP[i] + 1) << (Level*SizePower)) - p[i])*dT[i];
                iStep[i] = 1;
            } else {
                iP   [i] = corner[i] + min(iP[i] - corner[i], BrickSize - 1);
                nextT[i] = tMin + (p[i] - float(iP[i] << (Level*SizePower)))*dT[i];
                iStep[i] = -1;
            }
            iP[i] &= (1 << SizePower) - 1;
        }

        Vec3f tStep = dT*float(1 << (Level*SizePower));

        while (tMin < tMax) {
            ElementType element = cube.at(iP.x(), iP.y(), iP.z());

            if (element) {
                if (Level != 0) {
                    if (dda<Level == 0 ? 0 : Level - 1>(_grids[Level - 1][element - 1], o, dir, tMin, tMax, dT,
                           (corner + iP) << SizePower, intersect))
                        return true;
                } else {
                    if (intersect(element - 1, _offset + Vec3f(corner + iP)))
                        return true;
                }
            }

            int minIdx = nextT.minDim();
            tMin = nextT[minIdx];

            nextT[minIdx] += tStep[minIdx];
            iP   [minIdx] += iStep[minIdx];

            if (iP[minIdx] < 0 || iP[minIdx] >= BrickSize)
                return false;
        }

        return false;
    }

public:
    VoxelHierarchy(Vec3f offset, const ElementType *data)
    : _offset(offset)
    {
        int temporarySize = 1 << 3*((NumLevels - 1)*SizePower);
        std::unique_ptr<ElementType[]> temporaryA(new ElementType[temporarySize]);
        std::unique_ptr<ElementType[]> temporaryB(new ElementType[temporarySize]);

        ElementType *bufferA = temporaryA.get(), *bufferB = temporaryB.get();

        _grids[NumLevels - 1].reset(new Cubelet[1]);

        for (int i = 0; i < NumLevels - 1; ++i) {
            const ElementType *src = (i == 0 ? data : bufferA);
            ElementType *dst = (i == NumLevels - 2 ? &_grids[NumLevels - 1][0].at(0, 0, 0) : bufferB);
            buildHierarchy(i, src, dst);
            std::swap(bufferA, bufferB);
        }
    }

    template<typename LAMBDA>
    bool trace(Ray &ray, LAMBDA intersect) const
    {
        Vec3f dT = 1.0f/ray.dir();

        Vec3f o = ray.pos() - _offset;

        Vec3f relMin(-o);
        Vec3f relMax(float(1 << (NumLevels*SizePower)) - o);

        float tMin = ray.nearT(), tMax = ray.farT();
        Vec3f tMins;
        for (int i = 0; i < 3; ++i) {
            if (dT[i] >= 0.0f) {
                tMin = max(tMin, relMin[i]*dT[i]);
                tMax = min(tMax, relMax[i]*dT[i]);

                tMins[i] = relMin[i]*dT[i];
            } else {
                tMax = min(tMax, relMin[i]*dT[i]);
                tMin = max(tMin, relMax[i]*dT[i]);

                tMins[i] = relMax[i]*dT[i];
            }
        }

        if (tMin >= tMax)
            return false;

        return dda<NumLevels - 1>(_grids[NumLevels - 1][0], o, ray.dir(), tMin, tMax, std::abs(dT), Vec3i(0), intersect);
    }
};

}



#endif /* VOXELHIERARCHY_HPP_ */
