#pragma once

#include <openxr/openxr.h>

#include <wnvr/protocol/messages.h>

// OpenXR の姿勢（位置と向き）の計算。必要なものだけを持つ。
namespace wnvr::quest {

inline XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline XrQuaternionf Conjugate(const XrQuaternionf& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

inline XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
    const auto rotated = Multiply(Multiply(q, {v.x, v.y, v.z, 0}), Conjugate(q));
    return {rotated.x, rotated.y, rotated.z};
}

// 姿勢 a の座標系の中にある姿勢 b を、a の親の座標系で表す。
inline XrPosef Compose(const XrPosef& a, const XrPosef& b) {
    const auto offset = Rotate(a.orientation, b.position);
    return {Multiply(a.orientation, b.orientation),
        {a.position.x + offset.x, a.position.y + offset.y, a.position.z + offset.z}};
}

inline XrPosef Inverse(const XrPosef& pose) {
    const auto inverse = Conjugate(pose.orientation);
    return {inverse, Rotate(inverse, {-pose.position.x, -pose.position.y, -pose.position.z})};
}

inline XrPosef ToXrPose(const protocol::Vec3& p, const protocol::Quat& q) {
    return {{q.x, q.y, q.z, q.w}, {p.x, p.y, p.z}};
}

}
