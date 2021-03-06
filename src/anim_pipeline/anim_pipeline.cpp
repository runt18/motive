// Copyright 2015 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Suppress warnings in external header.
#pragma warning(push)            // for Visual Studio
#pragma warning(disable : 4068)  // "unknown pragma" -- for Visual Studio
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <fbxsdk.h>
#pragma GCC diagnostic pop
#pragma warning(pop)

#include <assert.h>
#include <fstream>
#include <functional>
#include <stdarg.h>
#include <stdio.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "anim_generated.h"
#include "fplutil/file_utils.h"
#include "mathfu/glsl_mappings.h"
#include "motive/anim.h"
#include "motive/common.h"
#include "motive/init.h"
#include "motive/math/angle.h"

namespace motive {

using motive::MatrixOperationType;
using motive::kInvalidMatrixOperation;
using motive::kNumMatrixOperationTypes;
using motive::BoneIndex;
using motive::kInvalidBoneIdx;
using std::string;

enum RepeatPreference {
  kRepeatIfRepeatable,
  kAlwaysRepeat,
  kNeverRepeat
};

static const int kTimeGranularityMiliseconds = 10;
static const int kDefaultChannelOrder[] = {0, 1, 2};
static const int kRotationOrderToChannelOrder[][3] = {
    {2, 1, 0},  // eOrderXYZ,
    {2, 0, 1},  // eOrderXZY,
    {1, 0, 2},  // eOrderYZX,
    {1, 2, 0},  // eOrderYXZ,
    {0, 2, 1},  // eOrderZXY,
    {0, 1, 2},  // eOrderZYX,
    {2, 1, 0},  // eOrderSphericXYZ
};

// Each log message is given a level of importance.
// We only output messages that have level >= our current logging level.
enum LogLevel {
  kLogVerbose,
  kLogInfo,
  kLogImportant,
  kLogWarning,
  kLogError,
  kNumLogLevels
};

// Prefix log messages at this level with this message.
static const char* kLogPrefix[] = {"",           // kLogVerbose
                                   "",           // kLogInfo
                                   "",           // kLogImportant
                                   "Warning: ",  // kLogWarning
                                   "Error: "     // kLogError
};
static_assert(MOTIVE_ARRAY_SIZE(kLogPrefix) == kNumLogLevels,
              "kLogPrefix length is incorrect");

/// @class Logger
/// @brief Output log messages if they are above an adjustable threshold.
class Logger {
 public:
  Logger() : level_(kLogImportant) {}

  void set_level(LogLevel level) { level_ = level; }
  LogLevel level() const { return level_; }

  /// Output a printf-style message if our current logging level is
  /// >= `level`.
  void Log(LogLevel level, const char* format, ...) const {
    if (level < level_) return;

    // Prefix message with log level, if required.
    const char* prefix = kLogPrefix[level];
    if (prefix[0] != '\0') {
      printf("%s", prefix);
    }

    // Redirect output to stdout.
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
  }

 private:
  LogLevel level_;
};

// Half a percent.
static const float kDefaultScaleTolerance = 0.005f;

// 0.5 degrees in radians.
static const float kDefaultRotateTolerance = 0.00873f;

// Totally arbitrary. TODO: make a percentage of the model size.
static const float kDefaultTranslateTolerance = 0.01f;

// 0.5 degrees in radians.
static const float kDefaultDerivativeAngleTolerance = 0.00873f;

// 10 degrees in radians.
static const float kDefaultRepeatDerivativeAngleTolerance = 0.1745f;

// Use these bitfields to find situations where scale x, y, and z occur, in
// any order, in a row.
static const uint32_t kScaleXBitfield = 1 << motive::kScaleX;
static const uint32_t kScaleYBitfield = 1 << motive::kScaleY;
static const uint32_t kScaleZBitfield = 1 << motive::kScaleZ;
static const uint32_t kScaleXyzBitfield =
    kScaleXBitfield | kScaleYBitfield | kScaleZBitfield;

/// @brief Convert derivative to its angle in x/y space.
///  derivative 0 ==> angle 0
///  derivative 1 ==> angle 45 degrees
///  derivative +inf ==> angle 90 degrees
///  derivative -2 ==> angle -63.4 degrees
/// @returns Angle, in radians, >= -pi and <= pi
static inline float DerivativeAngle(float derivative) {
  return atan(derivative);
}

/// @brief Amount the output curves are allowed to deviate from the input
///        curves.
struct Tolerances {
  /// Amount output scale curves can deviate, unitless.
  float scale;

  /// Amount output rotate curves can deviate, in radians.
  float rotate;

  /// Amount output translate curves can deviate, in scene's distance units.
  float translate;

  /// Amount derivative--converted to an angle in x/y--can deviate, in radians.
  float derivative_angle;

  /// Amount derivative--converted to an angle in x/y--can deviate, in radians,
  /// This value used when determining if an animation repeats or not.
  float repeat_derivative_angle;

  Tolerances()
      : scale(kDefaultScaleTolerance),
        rotate(kDefaultRotateTolerance),
        translate(kDefaultTranslateTolerance),
        derivative_angle(kDefaultDerivativeAngleTolerance),
        repeat_derivative_angle(kDefaultRepeatDerivativeAngleTolerance) {}
};

// Unique id identifying a single float curve being animated.
typedef int FlatChannelId;

// Time used for animation curves. Use an integer type for time so that we
// don't loose precision at the end of long animations.
typedef int FlatTime;

// Value output from animation curves.
typedef float FlatVal;

// Slope of animation curves.
typedef float FlatDerivative;

/// @class FlatAnim
/// @brief Hold animation data to be written to FlatBuffer animation format.
class FlatAnim {
 public:
  explicit FlatAnim(const Tolerances& tolerances, Logger& log)
      : tolerances_(tolerances), log_(log) {}

  void AllocBone(const char* bone_name, int depth) {
    bones_.push_back(Bone(bone_name, depth));
  }

  FlatChannelId AllocChannel(MatrixOperationType op) {
    Channels& channels = CurChannels();
    channels.push_back(Channel(op));
    return static_cast<FlatChannelId>(channels.size() - 1);
  }

  void AddConstant(FlatChannelId channel_id, FlatVal const_val) {
    Channels& channels = CurChannels();
    Nodes& n = channels[channel_id].nodes;
    n.resize(0);
    n.push_back(SplineNode(0, const_val, 0.0f));
  }

  void AddCurve(FlatChannelId channel_id, FlatTime time_start,
                FlatTime time_end, const FlatVal* vals,
                const FlatDerivative* derivatives, size_t count) {
    // Create cubic that covers the entire range from time_start ~ time_end.
    // The cubic `c` is shifted to the left, to start at 0 instead of
    // time_start.
    // This is to maintain floating-point precision.
    const float time_width = static_cast<float>(time_end - time_start);
    const CubicCurve c(CubicInit(vals[0], derivatives[0], vals[count - 1],
                                 derivatives[count - 1], time_width));

    // Find the worst intermediate val in for this cubic.
    // That is, the index into `vals` where the cubic evaluation is most
    // inaccurate.
    const float time_inc = time_width / (count - 1);
    float time = time_inc;
    float worst_diff = 0.0f;
    float worst_time = 0.0f;
    size_t worst_idx = 0;
    for (size_t i = 1; i < count - 1; ++i) {
      const float cubic_val = c.Evaluate(time);
      const float curve_val = vals[i];
      const float diff_val = fabs(cubic_val - curve_val);
      if (diff_val > worst_diff) {
        worst_idx = i;
        worst_diff = diff_val;
        worst_time = time;
      }
      time += time_inc;
    }

    // If the cubic is off by a lot, divide the curve into two curves at the
    // worst time. Note that the recursion will end, at worst, when count ==> 2.
    const float tolerance = Tolerance(channel_id);
    if (worst_idx > 0 && worst_diff > tolerance) {
      const FlatTime time_mid = time_start + static_cast<FlatTime>(worst_time);
      AddCurve(channel_id, time_start, time_mid, vals, derivatives,
               worst_idx + 1);
      AddCurve(channel_id, time_mid, time_end, &vals[worst_idx],
               &derivatives[worst_idx], count - worst_idx);
      return;
    }

    // Otherwise, the generated cubic is good enough, so record it.
    const SplineNode start_node(time_start, vals[0], derivatives[0]);
    const SplineNode end_node(time_end, vals[count - 1],
                              derivatives[count - 1]);

    // Only push the start node if it differs from the previously pushed end
    // node. Most of the time it will be the same.
    Channels& channels = CurChannels();
    Nodes& n = channels[channel_id].nodes;
    const bool start_matches_prev = n.size() > 0 && n.back() == start_node;
    if (!start_matches_prev) {
      n.push_back(start_node);
    }
    n.push_back(end_node);
  }

  /// @brief Remove redundant nodes from `channel_id`.
  void PruneNodes(FlatChannelId channel_id) {
    const float tolerance = Tolerance(channel_id);

    // For every node try to prune as many redunant nodes that come after it.
    // A node is redundant if the spline evaluates to the same value even if
    // it doesn't exists (note: here "same value" means within `tolerances_`).
    Channels& channels = CurChannels();
    Nodes& n = channels[channel_id].nodes;
    std::vector<bool> prune(n.size(), false);
    for (size_t i = 0; i < n.size();) {
      size_t next_i = i + 1;
      for (size_t j = i + 2; j < n.size(); ++j) {
        const bool redundant =
            IntermediateNodesRedundant(&n[i], j - i + 1, tolerance);
        if (redundant) {
          prune[j - 1] = true;
          next_i = j;
        }
      }
      i = next_i;
    }

    // Compact to remove all pruned nodes.
    size_t write = 0;
    for (size_t read = 0; read < n.size(); ++read) {
      if (prune[read]) continue;
      if (write < read) {
        n[write] = n[read];
      }
      write++;
    }
    n.resize(write);

    // If value is constant for the entire time, remove the second node so that
    // we know to output a constant value in `OutputFlatBuffer()`.
    const bool is_const =
        n.size() == 2 && fabs(n[0].val - n[1].val) < tolerance &&
        fabs(DerivativeAngle(n[0].derivative)) < tolerances_.derivative_angle &&
        fabs(DerivativeAngle(n[1].derivative)) < tolerances_.derivative_angle;
    if (is_const) {
      n.resize(1);
    }
  }

  /// @brief Collapse multiple channels into one, when possible.
  void PruneChannels() {
    for (auto bone = bones_.begin(); bone != bones_.end(); ++bone) {
      // Iterate from the end to minimize the cost of the erase operations.
      Channels& channels = bone->channels;
      for (FlatChannelId ch = static_cast<FlatChannelId>(channels.size() - 1);
           ch >= 0; ch--) {
        // Collapse kScaleX,Y,Z into kScaleUniformly.
        const bool uniform_scale = UniformScaleChannels(channels, ch);
        if (uniform_scale) {
          log_.Log(kLogVerbose,
                   "  Collapsing scale x, y, z channels %d~%d into"
                   " one scale-uniformly channel\n",
                   ch, ch + 2);

          channels[ch].op = motive::kScaleUniformly;
          channels.erase(channels.begin() + (ch + 1),
                         channels.begin() + (ch + 3));
        }
      }
    }
  }

  void LogChannel(FlatChannelId channel_id) const {
    const Channels& channels = CurChannels();
    const Nodes& n = channels[channel_id].nodes;
    for (size_t i = 0; i < n.size(); ++i) {
      const SplineNode& node = n[i];
      log_.Log(kLogVerbose, "    flat, %d, %d, %f, %f\n", i, node.time,
               node.val, node.derivative);
    }
  }

  void LogAllChannels() const {
    log_.Log(kLogInfo, "  %30s %16s  %9s   %s\n", "bone name", "operation",
             "time range", "values");
    for (BoneIndex bone_idx = 0; bone_idx < bones_.size(); ++bone_idx) {
      const Bone& bone = bones_[bone_idx];
      const Channels& channels = bone.channels;
      if (channels.size() == 0) continue;

      for (auto c = channels.begin(); c != channels.end(); ++c) {
        log_.Log(kLogInfo, "  %30s %16s   ", BoneBaseName(bone.name),
                 MatrixOpName(c->op));
        const char* format =
            motive::RotateOp(c->op) ? "%.0f " : motive::TranslateOp(c->op)
                                                    ? "%.1f "
                                                    : "%.2f ";
        const float factor =
            motive::RotateOp(c->op) ? motive::kRadiansToDegrees : 1.0f;

        const Nodes& n = c->nodes;
        if (n.size() <= 1) {
          log_.Log(kLogInfo, " constant   ");
        } else {
          log_.Log(kLogInfo, "%4d~%4d   ", n[0].time, n[n.size() - 1].time);
        }

        for (size_t i = 0; i < n.size(); ++i) {
          log_.Log(kLogInfo, format, factor * n[i].val);
        }
        log_.Log(kLogInfo, "\n");
      }
    }
  }

  bool OutputFlatBuffer(const string& output_file,
                        RepeatPreference repeat_preference) const {
    // Ensure output directory exists.
    const string output_dir = fplutil::DirectoryName(output_file);
    if (!fplutil::CreateDirectory(output_dir.c_str())) {
      log_.Log(kLogError, "Could not create output directory %s\n",
               output_dir.c_str());
      return false;
    }

    // All the channels should end at the same time.
    const FlatTime end_time = MaxAnimatedTime();

    flatbuffers::FlatBufferBuilder fbb;
    std::vector<flatbuffers::Offset<motive::MatrixAnimFb>> matrix_anims;
    std::vector<flatbuffers::Offset<flatbuffers::String>> bone_names;
    std::vector<BoneIndex> bone_parents;
    const size_t num_bones = bones_.size();
    matrix_anims.reserve(num_bones);
    bone_names.reserve(num_bones);
    bone_parents.reserve(num_bones);
    for (BoneIndex bone_idx = 0; bone_idx < num_bones; ++bone_idx) {
      const Bone& bone = bones_[bone_idx];
      const Channels& channels = bone.channels;

      // Output each channel as a MatrixOp, and gather in the `ops` vector.
      std::vector<flatbuffers::Offset<motive::MatrixOpFb>> ops;
      for (auto c = channels.begin(); c != channels.end(); ++c) {
        const Nodes& n = c->nodes;
        assert(n.size() > 0);

        flatbuffers::Offset<void> value;
        motive::MatrixOpValueFb value_type;
        if (n.size() <= 1) {
          // Output constant value MatrixOp.
          value = motive::CreateConstantOpFb(fbb, n[0].val).Union();
          value_type = motive::MatrixOpValueFb_ConstantOpFb;

        } else {
          // Output spline MatrixOp.
          CompactSpline s;
          CreateCompactSpline(*c, end_time, &s);
          value = CreateSplineFlatBuffer(fbb, s).Union();
          value_type = motive::MatrixOpValueFb_CompactSplineFb;
        }

        ops.push_back(motive::CreateMatrixOpFb(
            fbb, static_cast<motive::MatrixOperationTypeFb>(c->op), value_type,
            value));
      }

      // Convert vector into a FlatBuffers vector, and create the
      // MatrixAnimation.
      auto ops_fb = fbb.CreateVector(ops);
      auto matrix_anim_fb = CreateMatrixAnimFb(fbb, ops_fb);
      matrix_anims.push_back(matrix_anim_fb);
      bone_names.push_back(fbb.CreateString(bone.name));
      bone_parents.push_back(BoneParent(bone_idx));
    }

    // Finish off the FlatBuffer by creating the root RigAnimFb table.
    auto bone_names_fb = fbb.CreateVector(bone_names);
    auto bone_parents_fb = fbb.CreateVector(bone_parents);
    auto matrix_anims_fb = fbb.CreateVector(matrix_anims);
    const bool repeat = Repeat(repeat_preference);
    auto rig_anim_fb = CreateRigAnimFb(fbb, matrix_anims_fb, bone_parents_fb,
                                       bone_names_fb, repeat);
    motive::FinishRigAnimFbBuffer(fbb, rig_anim_fb);

    // Create the output file.
    FILE* file = fopen(output_file.c_str(), "wb");
    if (file == nullptr) {
      log_.Log(kLogError, "Could not open %s for writing\n",
               output_file.c_str());
      return false;
    }

    // Write the binary data to the file and close it.
    log_.Log(kLogVerbose, "Writing %s", output_file.c_str());
    fwrite(fbb.GetBufferPointer(), 1, fbb.GetSize(), file);
    fclose(file);

    // Log summary.
    log_.Log(kLogImportant, "  %s (%d bytes)\n",
             fplutil::RemoveDirectoryFromName(output_file).c_str(), NumBytes());
    return true;
  }

  float ToleranceForOp(MatrixOperationType op) const {
    return motive::RotateOp(op)
               ? tolerances_.rotate
               : motive::TranslateOp(op)
                     ? tolerances_.translate
                     : motive::ScaleOp(op) ? tolerances_.scale : 0.1f;
  }

  float ToleranceForDerivativeAngle() const {
    return tolerances_.derivative_angle;
  }

  bool IsDefaultValue(MatrixOperationType op, float value) const {
    return fabs(value - DefaultOpValue(op)) < ToleranceForOp(op);
  }

  int NumBytes() const {
    static const size_t kBytesPerSplineNode = 6;
    size_t num_bytes =
        sizeof(motive::RigAnim) + bones_.size() * sizeof(motive::MatrixAnim);

    for (size_t i = 0; i < bones_.size(); ++i) {
      auto channels = bones_[i].channels;

      num_bytes += channels.size() * sizeof(motive::MatrixOperationInit);
      for (size_t j = 0; j < channels.size(); ++j) {
        Nodes nodes = channels[j].nodes;
        num_bytes += sizeof(CompactSpline) + nodes.size() * kBytesPerSplineNode;
      }
    }
    return static_cast<int>(num_bytes);
  }

 private:
  MOTIVE_DISALLOW_COPY_AND_ASSIGN(FlatAnim);

  struct SplineNode;
  struct Channel;
  typedef std::vector<SplineNode> Nodes;
  typedef std::vector<Channel> Channels;

  Channels& CurChannels() {
    assert(bones_.size() > 0);
    return bones_.back().channels;
  }
  const Channels& CurChannels() const {
    assert(bones_.size() > 0);
    return bones_.back().channels;
  }

  float Tolerance(FlatChannelId channel_id) const {
    const Channels& channels = CurChannels();
    return ToleranceForOp(channels[channel_id].op);
  }

  /// Return the time of the channel that requires the most time.
  FlatTime MaxAnimatedTime() const {
    FlatTime max_time = 0;
    for (auto bone = bones_.begin(); bone != bones_.end(); ++bone) {
      const Channels& channels = bone->channels;
      for (auto ch = channels.begin(); ch != channels.end(); ++ch) {
        if (ch->nodes.size() > 0) {
          max_time = std::max(max_time, ch->nodes.back().time);
        }
      }
    }
    return max_time;
  }

  /// Return the first channel of the first bone that isn't repeatable.
  /// If all channels are repeatable, return kInvalidBoneIdx.
  /// A channel is repeatable if its start and end values and derivatives
  /// are within `tolerances_`.
  BoneIndex FirstNonRepeatingBone(FlatChannelId* first_channel_id) const {
    for (BoneIndex bone_idx = 0; bone_idx < bones_.size(); ++bone_idx) {
      const Bone& bone = bones_[bone_idx];
      const Channels& channels = bone.channels;

      for (FlatChannelId channel_id = 0;
           channel_id < static_cast<FlatChannelId>(channels.size());
           ++channel_id) {
        const Channel& channel = channels[channel_id];

        // Get deltas for the start and end of the channel.
        const SplineNode& start = channel.nodes.front();
        const SplineNode& end = channel.nodes.back();
        const float diff_val = fabs(start.val - end.val);
        const float diff_derivative_angle =
            fabs(DerivativeAngle(start.derivative - end.derivative));

        // Return false unless the start and end of the channel are the same.
        const float tolerance = ToleranceForOp(channel.op);
        const bool same =
            diff_val < tolerance &&
            diff_derivative_angle < tolerances_.repeat_derivative_angle;
        if (!same) {
          *first_channel_id = channel_id;
          return bone_idx;
        }
      }
    }
    return kInvalidBoneIdx;
  };

  // Determine if the animation should repeat back to start after it reaches
  // the end.
  bool Repeat(RepeatPreference repeat_preference) const {
    if (repeat_preference == kNeverRepeat) return false;

    // Check to see if the animation is repeatable.
    FlatChannelId channel_id = 0;
    const BoneIndex bone_idx = FirstNonRepeatingBone(&channel_id);
    const bool repeat = repeat_preference == kAlwaysRepeat ||
                        (repeat_preference == kRepeatIfRepeatable &&
                         bone_idx == kInvalidBoneIdx);

    // Log repeat information.
    if (repeat_preference == kAlwaysRepeat) {
      if (bone_idx != kInvalidBoneIdx) {
        const Bone& bone = bones_[bone_idx];
        const Channel& channel = bone.channels[channel_id];
        log_.Log(kLogWarning,
                 "Animation marked as repeating (as requested),"
                 " but it does not repeat on bone %s's"
                 " `%s` channel\n",
                 bone.name.c_str(), MatrixOpName(channel.op));
      }
    } else if (repeat_preference == kRepeatIfRepeatable) {
      log_.Log(kLogVerbose, repeat ? "Animation repeats.\n"
                                   : "Animation does not repeat.\n");
    }

    return repeat;
  }

  /// @brief Return true if the three channels starting at `channel_id`
  ///        can be replaced with a single kScaleUniformly channel.
  bool UniformScaleChannels(const Channels& channels,
                            FlatChannelId channel_id) const {
    if (channel_id + 2 >= static_cast<FlatChannelId>(channels.size()))
      return false;

    // Consider the three channels starting at `channel_id`.
    const Channel& c0 = channels[channel_id];
    const Channel& c1 = channels[channel_id + 1];
    const Channel& c2 = channels[channel_id + 2];

    // The order is not important, but we need kScaleX, Y, and Z.
    const uint32_t op_bits = (1 << c0.op) | (1 << c1.op) | (1 << c2.op);
    if (op_bits != kScaleXyzBitfield) return false;

    // The sequence of values must also be identical.
    const Nodes& n0 = c0.nodes;
    const Nodes& n1 = c1.nodes;
    const Nodes& n2 = c2.nodes;
    const bool same_length = n0.size() == n1.size() && n0.size() == n2.size() &&
                             n1.size() == n2.size();
    if (!same_length) return false;

    // The splines must be equal.
    const float tolerance = tolerances_.scale;
    for (size_t i = 0; i < n0.size(); ++i) {
      const SplineNode v0 = n0[i];
      const SplineNode v1 = n1[i];
      const SplineNode v2 = n2[i];
      const bool are_equal =
          EqualNodes(v0, v1, tolerance, tolerances_.derivative_angle) &&
          EqualNodes(v0, v2, tolerance, tolerances_.derivative_angle) &&
          EqualNodes(v1, v2, tolerance, tolerances_.derivative_angle);
      if (!are_equal) return false;
    }

    return true;
  }

  BoneIndex BoneParent(int bone_idx) const {
    // If at top level, there is no parent, so return
    const int bone_depth = bones_[bone_idx].depth;
    if (bone_depth == 0) return kInvalidBoneIdx;

    // `bones_` are in depth-first order, so the parent is the previous
    // non-sibling.
    for (int i = bone_idx - 1; i >= 0; --i) {
      assert(bones_[i].depth >= bone_depth - 1);
      if (bones_[i].depth < bone_depth) return static_cast<BoneIndex>(i);
    }
    assert(false);
    return kInvalidBoneIdx;
  }

  /// @brief Returns true if all nodes between the first and last in `n`
  ///        can be deleted without noticable difference to the curve.
  bool IntermediateNodesRedundant(const SplineNode* n, size_t len,
                                  float tolerance) const {
    // If the start and end nodes occur at the same time and are equal,
    // then ignore everything inbetween them.
    const SplineNode& start = n[0];
    const SplineNode& end = n[len - 1];
    if (EqualNodes(start, end, tolerance, tolerances_.derivative_angle))
      return true;

    // Construct cubic curve `c` that skips all the intermediate nodes.
    const float cubic_width = static_cast<float>(end.time - start.time);
    const CubicCurve c(CubicInit(start.val, start.derivative, end.val,
                                 end.derivative, cubic_width));

    // For each intermediate node, check if the cubic `c` is close.
    for (size_t i = 1; i < len - 1; ++i) {
      // Evaluate `c` at the time of `mid`.
      const SplineNode& mid = n[i];
      const float mid_time = static_cast<float>(mid.time - start.time);
      const float mid_val = c.Evaluate(mid_time);
      const float mid_derivative = c.Derivative(mid_time);

      // If the mid point is on the curve, it's redundant.
      const float derivative_angle_error =
          DerivativeAngle(mid_derivative - mid.derivative);
      const bool mid_on_c =
          fabs(mid_val - mid.val) < tolerance &&
          fabs(derivative_angle_error) < tolerances_.derivative_angle;
      if (!mid_on_c) return false;
    }

    // All mid points are redundant.
    return true;
  }

  // Remove the namespacing from the bone name.
  static const char* BoneBaseName(const string& name) {
    const size_t colon = name.find_last_of(':');
    const size_t base_idx = colon == string::npos ? 0 : colon + 1;
    return &name[base_idx];
  }

  static bool EqualNodes(const SplineNode& a, const SplineNode& b,
                         float tolerance, float derivative_tolerance) {
    return a.time == b.time && fabs(a.val - a.val) < tolerance &&
           fabs(DerivativeAngle(a.derivative - b.derivative)) <
               derivative_tolerance;
  }

  static bool ConstOp(const Channel& c) { return c.nodes.size() <= 1; }

  static bool ModularOp(MatrixOperationType op) { return motive::RotateOp(op); }

  static FlatVal DefaultOpValue(MatrixOperationType op) {
    // Translate and rotate operations are 0 by default.
    // Scale operations are 1 by default.
    return motive::ScaleOp(op) ? 1.0f : 0.0f;
  }

  static flatbuffers::Offset<motive::CompactSplineFb> CreateSplineFlatBuffer(
      flatbuffers::FlatBufferBuilder& fbb, const CompactSpline& s) {
    auto nodes_fb = fbb.CreateVectorOfStructs(
        reinterpret_cast<const motive::CompactSplineNodeFb*>(s.nodes()),
        s.NumNodes());

    auto spline_fb = motive::CreateCompactSplineFb(fbb, s.y_range().start(),
                                                   s.y_range().end(),
                                                   s.x_granularity(), nodes_fb);

    return spline_fb;
  }

  static Range SplineYRange(const Channel& ch) {
    // Find extreme values for nodes.
    Range y_range(Range::Empty());
    for (auto n = ch.nodes.begin(); n != ch.nodes.end(); ++n) {
      y_range = y_range.Include(n->val);
    }
    return y_range;
  }

  static void CreateCompactSpline(const Channel& ch, FlatTime end_time,
                                  CompactSpline* s) {
    const Nodes& nodes = ch.nodes;
    assert(nodes.size() > 1);

    // Maximize the bits we get for x by making the last time the maximum
    // x-value.
    const float x_granularity =
        CompactSpline::RecommendXGranularity(static_cast<float>(end_time));
    const Range y_range = SplineYRange(ch);

    // Construct the Spline from the node data directly.
    s->Init(y_range, x_granularity, static_cast<int>(nodes.size()));
    for (auto n = nodes.begin(); n != nodes.end(); ++n) {
      s->AddNode(static_cast<float>(n->time), n->val, n->derivative,
                 kAddWithoutModification);
    }

    // Append one more node so that all channels end at the same time.
    const SplineNode& back_node = nodes.back();
    if (back_node.time < end_time) {
      s->AddNode(static_cast<float>(end_time), back_node.val, 0.0f,
                 kAddWithoutModification);
    }
  }

  struct SplineNode {
    FlatTime time;
    FlatVal val;
    FlatDerivative derivative;
    SplineNode() : time(0), val(0.0f), derivative(0.0f) {}
    SplineNode(FlatTime time, FlatVal val, FlatDerivative derivative)
        : time(time), val(val), derivative(derivative) {}
    bool operator==(const SplineNode& rhs) const {
      return time == rhs.time && val == rhs.val && derivative == rhs.derivative;
    }
    bool operator!=(const SplineNode& rhs) const { return !operator==(rhs); }
  };

  struct Channel {
    MatrixOperationType op;
    Nodes nodes;

    Channel() : op(kInvalidMatrixOperation) {}
    explicit Channel(MatrixOperationType op) : op(op) {}
  };

  struct Bone {
    // Unique name for this bone. Taken from mesh hierarchy.
    string name;

    // Hierarchy depth. From this we can derive the tree, since bones are
    // listed in depth-first order.
    int depth;

    // Hold animation data. One curve per channel.
    Channels channels;

    Bone(const char* name, int depth) : name(name), depth(depth) {
      // There probably won't be more than one of each op type.
      channels.reserve(kNumMatrixOperationTypes);
    }
  };

  // Hold animation data for each bone that's animated.
  std::vector<Bone> bones_;

  // Amount output curves are allowed to deviate from input.
  Tolerances tolerances_;

  // Information and warnings.
  Logger& log_;
};

struct ChannelNameToMatrixOp {
  const char* name;
  MatrixOperationType op;
};

/// @class FbxParser
/// @brief Load FBX files and save their geometry and animations in our
///        FlatBuffer format.
class FbxAnimParser {
 public:
  explicit FbxAnimParser(Logger& log)
      : manager_(nullptr), scene_(nullptr), log_(log) {
    // The FbxManager is the gateway to the FBX API.
    manager_ = FbxManager::Create();
    if (manager_ == nullptr) {
      log_.Log(kLogError, "Unable to create FBX manager.\n");
      return;
    }

    // Initialize with standard IO settings.
    FbxIOSettings* ios = FbxIOSettings::Create(manager_, IOSROOT);
    manager_->SetIOSettings(ios);

    // Create an FBX scene. This object holds most objects imported/exported
    // from/to files.
    scene_ = FbxScene::Create(manager_, "My Scene");
    if (scene_ == nullptr) {
      log_.Log(kLogError, "Unable to create FBX scene.\n");
      return;
    }
  }

  ~FbxAnimParser() {
    // Delete the FBX Manager and all objects that it created.
    if (manager_ != nullptr) manager_->Destroy();
  }

  bool Valid() const { return manager_ != nullptr && scene_ != nullptr; }

  bool Load(const char* file_name) {
    if (!Valid()) return false;

    log_.Log(
        kLogInfo,
        "---- anim_pipeline: %s ------------------------------------------\n",
        fplutil::BaseFileName(file_name).c_str());

    // Create the importer and initialize with the file.
    FbxImporter* importer = FbxImporter::Create(manager_, "");
    const bool init_status =
        importer->Initialize(file_name, -1, manager_->GetIOSettings());

    // Check the SDK and pipeline versions.
    int sdk_major = 0, sdk_minor = 0, sdk_revision = 0;
    int file_major = 0, file_minor = 0, file_revision = 0;
    FbxManager::GetFileFormatVersion(sdk_major, sdk_minor, sdk_revision);
    importer->GetFileVersion(file_major, file_minor, file_revision);

    // Report version information.
    log_.Log(kLogVerbose, "File version %d.%d.%d, SDK version %d.%d.%d\n",
             file_major, file_minor, file_revision, sdk_major, sdk_minor,
             sdk_revision);

    // Exit on load error.
    if (!init_status) {
      FbxString error = importer->GetStatus().GetErrorString();
      log_.Log(kLogError, "%s\n\n", error.Buffer());
      return false;
    }
    if (!importer->IsFBX()) {
      log_.Log(kLogError, "Not an FBX file\n\n");
      return false;
    }

    // Import the scene.
    const bool import_status = importer->Import(scene_);

    // Clean-up temporaries.
    importer->Destroy();

    // Exit if the import failed.
    if (!import_status) return false;

    // Get global scale from the internal units.
    global_scale_ =
        1.0 / scene_->GetGlobalSettings().GetSystemUnit().GetScaleFactor();
    log_.Log(kLogVerbose, "Scene scale factor is %f\n", global_scale_);

    // Remember the source file name so we can search for textures nearby.
    anim_file_name_ = string(file_name);
    return true;
  }

  void GatherFlatAnim(FlatAnim* out) const {
    // Initial depth is -1 since root node is always ignored.
    GatherFlatAnimRecursive(scene_->GetRootNode(), -1, out);
  }

  // For debugging. Very useful to output the local transform matrices (and
  // component values) at a given time of the animation. We can compare these
  // to the runtime values, if something doesn't match up.
  void LogAnimStateAtTime(int time_in_ms) const {
    FbxTime time;
    time.SetMilliSeconds(time_in_ms);
    LogAnimStateAtTimeRecursive(scene_->GetRootNode(), time);
  }

 private:
  MOTIVE_DISALLOW_COPY_AND_ASSIGN(FbxAnimParser);

  struct AnimProperty {
    FbxPropertyT<FbxDouble3>* property;
    motive::MatrixOperationType x_op;
  };

  void LogIfNotEqual(const FbxVector4& v, const FbxVector4& compare,
                     const char* name) const {
    if (v == compare) return;
    log_.Log(kLogInfo, "%s: (%6.2f %6.2f %6.2f %6.2f)\n", name, v[0], v[1],
             v[2], v[3]);
  }

  // For each mesh in the tree of nodes under `node`, add a surface to `out`.
  void LogAnimStateAtTimeRecursive(FbxNode* node, const FbxTime& time) const {
    // We're only interested in mesh nodes. If a node and all nodes under it
    // have no meshes, we early out.
    if (node == nullptr || !NodeHasMesh(node)) return;
    log_.Log(kLogInfo, "Node: %s\n", node->GetName());

    // Log local transform. It's an affine transform so is 4x3.
    const FbxAMatrix& local = node->EvaluateLocalTransform(time);
    for (int i = 0; i < 3; ++i) {
      const FbxVector4 r = local.GetColumn(i);
      log_.Log(kLogInfo, "  (%6.2f %6.2f %6.2f %6.2f)\n", r[0], r[1], r[2],
               r[3]);
    }

    // Log the components of the local transform, but only if they don't
    // match their default values.
    const FbxVector4 zero(0.0, 0.0, 0.0, 0.0);
    const FbxVector4 one(1.0, 1.0, 1.0, 1.0);
    LogIfNotEqual(node->EvaluateLocalTranslation(time), zero, "translate");
    LogIfNotEqual(node->GetRotationOffset(FbxNode::eSourcePivot), zero,
                  "rotation_offset");
    LogIfNotEqual(node->GetRotationPivot(FbxNode::eSourcePivot), zero,
                  "rotation_pivot");
    LogIfNotEqual(node->GetPreRotation(FbxNode::eSourcePivot), zero,
                  "pre_rotation");
    LogIfNotEqual(node->EvaluateLocalRotation(time), zero, "rotate");
    LogIfNotEqual(node->GetPostRotation(FbxNode::eSourcePivot), zero,
                  "post_rotation");
    LogIfNotEqual(node->GetScalingOffset(FbxNode::eSourcePivot), zero,
                  "scaling_offset");
    LogIfNotEqual(node->GetScalingPivot(FbxNode::eSourcePivot), zero,
                  "scaling_pivot");
    LogIfNotEqual(node->EvaluateLocalScaling(time), one, "scaling");
    LogIfNotEqual(node->GetGeometricTranslation(FbxNode::eSourcePivot), zero,
                  "geometric_translation");
    LogIfNotEqual(node->GetGeometricRotation(FbxNode::eSourcePivot), zero,
                  "geometric_rotation");
    LogIfNotEqual(node->GetGeometricScaling(FbxNode::eSourcePivot), one,
                  "geometric_scaling");
    log_.Log(kLogInfo, "\n");

    // Recursively traverse each node in the scene
    for (int i = 0; i < node->GetChildCount(); i++) {
      LogAnimStateAtTimeRecursive(node->GetChild(i), time);
    }
  }

  static FlatTime FbxToFlatTime(const FbxTime& t) {
    const FbxLongLong milliseconds = t.GetMilliSeconds();
    assert(milliseconds <= std::numeric_limits<FlatTime>::max());
    return static_cast<FlatTime>(milliseconds);
  }

  FlatVal FbxToFlatValue(const double x, const MatrixOperationType op) const {
    return motive::RotateOp(op) ? static_cast<float>(FBXSDK_DEG_TO_RAD * x)
                                : motive::TranslateOp(op)
                                      ? static_cast<FlatVal>(global_scale_ * x)
                                      : static_cast<float>(x);
  }

  FlatDerivative FbxToFlatDerivative(const float d,
                                     const MatrixOperationType op) const {
    // The FBX derivative appears to be in units of seconds.
    // The FlatBuffer file format is in units of milliseconds.
    const float d_time_scaled = d / 1000.0f;
    return FbxToFlatValue(d_time_scaled, op);
  }

  void GatherFlatAnimRecursive(FbxNode* node, int depth, FlatAnim* out) const {
    // We're only interested in mesh nodes. If a node and all nodes under it
    // have no meshes, we early out.
    if (node == nullptr || !NodeHasMesh(node)) return;
    log_.Log(kLogVerbose, "Node: %s\n", node->GetName());

    // The root node cannot have a transform applied to it, so we do not
    // export it as a bone.
    if (node != scene_->GetRootNode()) {
      // Add a bone for this node, and gather the animation data that drives it.
      out->AllocBone(node->GetName(), depth);
      GatherFlatAnimForNode(node, out);
    }

    // Recursively traverse each node in the scene
    for (int i = 0; i < node->GetChildCount(); i++) {
      GatherFlatAnimRecursive(node->GetChild(i), depth + 1, out);
    }
  }

  static FbxAnimCurveNode* AnimCurveNodeDrivingProperty(FbxProperty& property) {
    const int count = property.GetSrcObjectCount();
    for (int i = 0; i < count; ++i) {
      FbxObject* obj = property.GetSrcObject(i);
      if (obj->GetClassId() == FbxAnimCurveNode::ClassId)
        return static_cast<FbxAnimCurveNode*>(obj);
    }
    return nullptr;
  }

  bool AnimConst(const AnimProperty& p, int channel, float tolerance,
                 float derivative_tolerance, FbxAnimCurveNode* anim_node,
                 float* const_value) const {
    // If anim_node can provide no data, return the value from the property.
    if (anim_node == nullptr ||
        channel >= static_cast<int>(anim_node->GetChannelsCount())) {
      *const_value = FbxToFlatValue(p.property->Get()[channel], p.x_op);
      return true;
    }

    // Grab the start value from the anim_node. If const, this will be the
    // constant value.
    *const_value =
        FbxToFlatValue(anim_node->GetChannelValue(channel, 0.0f), p.x_op);

    // If there is no animation curve, or the curve has no keys, then must be
    // const.
    FbxAnimCurve* curve = anim_node->GetCurve(channel);
    if (curve == nullptr || curve->KeyGetCount() <= 0) return true;

    // The first value may be different from the value at time 0.
    // The value at time 0 may actually be the end value, if the first key
    // doesn't start at time 0 and the channel cycles.
    const float first_value = FbxToFlatValue(curve->KeyGetValue(0), p.x_op);

    // If any keys has a different value, or non-zero slope, then not const.
    const int num_keys = curve->KeyGetCount();
    for (int i = 0; i < num_keys - 1; ++i) {
      const float left_derivative =
          FbxToFlatDerivative(curve->KeyGetLeftDerivative(i), p.x_op);
      const float right_derivative =
          FbxToFlatDerivative(curve->KeyGetRightDerivative(i), p.x_op);
      const float value = FbxToFlatValue(curve->KeyGetValue(i + 1), p.x_op);
      if (fabs(DerivativeAngle(left_derivative)) > derivative_tolerance ||
          fabs(DerivativeAngle(right_derivative)) > derivative_tolerance ||
          fabs(value - first_value) > tolerance)
        return false;
    }
    return true;
  }

  static const int* ChannelOrder(const FbxNode* node,
                                 const MatrixOperationType op) {
    // X,y,z order is significant only for rotations.
    if (!motive::RotateOp(op)) return kDefaultChannelOrder;

    // We output the last channel first, since they're applied in reverse
    // order.
    FbxEuler::EOrder rotation_order;
    node->GetRotationOrder(FbxNode::eSourcePivot, rotation_order);
    assert(0 <= rotation_order &&
           rotation_order < MOTIVE_ARRAY_SIZE(kRotationOrderToChannelOrder));
    return kRotationOrderToChannelOrder[rotation_order];
  }

  void GatherFlatAnimForNode(FbxNode* node, FlatAnim* out) const {
    // The FBX tranform format is defined as below (see
    // http://help.autodesk.com/view/FBX/2016/ENU/?guid=__files_GUID_10CDD63C_79C1_4F2D_BB28_AD2BE65A02ED_htm):
    //
    // WorldTransform = ParentWorldTransform * T * Roff * Rp * Rpre * R *
    //                  Rpost_inv * Rp_inv * Soff * Sp * S * Sp_inv
    //
    // But for now, we simplify as (see mesh_pipeline.cpp for details),
    //
    // WorldTransform = ParentWorldTransform * Roff * T * R * S
    //
    // TODO: Add in support for animating other properties. Not a big deal,
    //       just a few more lines of code in this function.
    const AnimProperty properties[] = {
        {&node->LclTranslation, motive::kTranslateX},
        {&node->LclRotation, motive::kRotateAboutX},
        {&node->LclScaling, motive::kScaleX}, };

    for (size_t i = 0; i < MOTIVE_ARRAY_SIZE(properties); ++i) {
      const AnimProperty& p = properties[i];

      // Skeletons have a fixed translate that cannot be animated.
      // Skeletal translates are applied in mesh_pipeline and output in the
      // constant bone transform.
      if (motive::TranslateOp(p.x_op) && node->GetSkeleton() != nullptr)
        continue;

      // Get the curve attached to the property that's animated.
      FbxAnimCurveNode* anim_node = AnimCurveNodeDrivingProperty(*p.property);
      if (anim_node == nullptr) continue;

      // Ensure we have three channels (x, y, z).
      const int num_channels = anim_node->GetChannelsCount();
      if (num_channels != 3) {
        log_.Log(kLogError, "Animation property has %d channels instead of 3\n",
                 num_channels);
        continue;
      }

      // Rotations must be applied in the correct order.
      const int* channel_order = ChannelOrder(node, p.x_op);
      for (int channel_idx = 0; channel_idx < 3; ++channel_idx) {
        // Proceed through each channel: x, y, z.
        const int channel = channel_order[channel_idx];
        const motive::MatrixOperationType op =
            static_cast<motive::MatrixOperationType>(p.x_op + channel);

        // If the channel is const, only output if it's not the default value.
        float const_value = 0.0f;
        const bool anim_const = AnimConst(p, channel, out->ToleranceForOp(op),
                                          out->ToleranceForDerivativeAngle(),
                                          anim_node, &const_value);
        if (anim_const && out->IsDefaultValue(op, const_value)) continue;

        // Allocate a channel_id for the output data.
        const FlatChannelId channel_id = out->AllocChannel(op);

        // Record constant value for this channel.
        if (anim_const) {
          out->AddConstant(channel_id, const_value);
          log_.Log(kLogVerbose, "  %s: constant %f\n", MatrixOpName(op),
                   const_value);
          continue;
        }

        // We process only the first curve, for simplicity.
        // If we run into animations with multiple curves, we should add
        // extra logic here.
        const int num_curves = anim_node->GetCurveCount(channel);
        if (num_curves > 1) {
          log_.Log(kLogWarning, "%s has %d curves. Only using the first one.\n",
                   MatrixOpName(op), num_curves);
        }

        // For every key in the curve, log data to `out`.
        log_.Log(kLogVerbose, "  %s: curve\n", MatrixOpName(op));
        FbxAnimCurve* curve = anim_node->GetCurve(channel);
        GatherFlatAnimCurve(channel_id, curve, op, out);
      }
    }

    // Collapse unnecesary channels, when possible.
    out->PruneChannels();
  }

  void GatherFlatAnimCurve(const FlatChannelId channel_id, FbxAnimCurve* curve,
                           const MatrixOperationType op, FlatAnim* out) const {
    log_.Log(kLogVerbose, "    source, key, x, y, slope\n");
    const int num_keys = curve->KeyGetCount();
    assert(num_keys > 1);  // Since we checked for constant values earlier.

    // If there are multiple keys, then add at least one cubic for each
    // key interval.
    for (int k = 0; k < num_keys - 1; ++k) {
      const FbxTime start_time = curve->KeyGetTime(k);
      const FbxTime end_time = curve->KeyGetTime(k + 1);

      // Gather indermediate values. We use these to check validity of cubic.
      static const int kNumIntermediateValues = 16;
      const FbxTime t_inc =
          (end_time - start_time) / (kNumIntermediateValues - 1);
      FbxTime t = start_time;
      int last_index = 0;
      float values[kNumIntermediateValues];
      float derivatives[kNumIntermediateValues];
      for (int i = 0; i < kNumIntermediateValues; ++i) {
        values[i] = FbxToFlatValue(curve->Evaluate(t, &last_index), op);
        derivatives[i] = FbxToFlatDerivative(
            curve->EvaluateLeftDerivative(t, &last_index), op);
        t += t_inc;
      }
      derivatives[0] = FbxToFlatDerivative(
          curve->EvaluateRightDerivative(start_time, &last_index), op);

      // Send to FlatAnim for conversion into cubic curves.
      const FlatTime start_time_flat = FbxToFlatTime(start_time);
      const FlatTime end_time_flat = FbxToFlatTime(end_time);
      out->AddCurve(channel_id, start_time_flat, end_time_flat, values,
                    derivatives, kNumIntermediateValues);

      // Log the input key points.
      log_.Log(kLogVerbose, "    fbx, %d, %d, %f, %f\n", k, start_time_flat,
               values[0], derivatives[0]);
      if (k == num_keys - 2) {
        log_.Log(kLogVerbose, "    fbx, %d, %d, %f, %f\n", k + 1, end_time_flat,
                 values[kNumIntermediateValues - 1],
                 derivatives[kNumIntermediateValues - 1]);
      }
    }

    // Remove duplicates.
    out->PruneNodes(channel_id);

    // Log the output key points.
    out->LogChannel(channel_id);
  }

  // Return true if `node` or any of its children has a mesh.
  static bool NodeHasMesh(FbxNode* node) {
    if (node->GetMesh() != nullptr) return true;

    // Recursively traverse each child node.
    for (int i = 0; i < node->GetChildCount(); i++) {
      if (NodeHasMesh(node->GetChild(i))) return true;
    }
    return false;
  }

  // Entry point to the FBX SDK.
  FbxManager* manager_;

  // Hold the FBX file data.
  FbxScene* scene_;

  // Distance scale for the entire scene. We need to multiply by this in order
  // to return to the authored units. Some programs like Maya store everything
  // in cm, even if you author it as m.
  double global_scale_;

  // Name of source mesh file. Used to search for textures, when the textures
  // are not found in their referenced location.
  string anim_file_name_;

  // Information and warnings.
  Logger& log_;
};

struct AnimPipelineArgs {
  AnimPipelineArgs()
      : fbx_file(""),
        output_file(""),
        log_level(kLogWarning),
        repeat_preference(kRepeatIfRepeatable),
        debug_time(-1) {}

  string fbx_file;        /// FBX input file to convert.
  string output_file;     /// File to write .fplanim to.
  LogLevel log_level;     /// Amount of logging to dump during conversion.
  Tolerances tolerances;  /// Amount output curves can deviate from input.
  RepeatPreference repeat_preference;  /// Loop back to start when reaches end.
  int debug_time;  /// If >0 output animation state at this time.
};

static bool ParseAnimPipelineArgs(int argc, char** argv, Logger& log,
                                  AnimPipelineArgs* args) {
  bool valid_args = true;

  // Last parameter is used as file name.
  if (argc > 1) {
    args->fbx_file = string(argv[argc - 1]);
    args->output_file = fplutil::RemoveExtensionFromName(args->fbx_file) + "." +
                        motive::RigAnimFbExtension();
  }

  // Ensure file name is valid.
  const bool valid_fbx_file =
      args->fbx_file.length() > 0 && args->fbx_file[0] != '-';
  if (!valid_fbx_file) {
    valid_args = false;
  }

  // Parse switches.
  for (int i = 1; i < argc - 1; ++i) {
    const string arg = argv[i];

    if (arg == "-v" || arg == "--verbose") {
      args->log_level = kLogVerbose;

    } else if (arg == "-d" || arg == "--details") {
      args->log_level = kLogImportant;

    } else if (arg == "-i" || arg == "--info") {
      args->log_level = kLogInfo;

    } else if (arg == "-o" || arg == "--out") {
      if (i + 1 < argc - 1) {
        args->output_file = string(argv[i + 1]);
        i++;
      } else {
        valid_args = false;
      }

    } else if (arg == "-s" || arg == "--scale") {
      if (i + 1 < argc - 1) {
        args->tolerances.scale = static_cast<float>(atof(argv[i + 1]));
        if (args->tolerances.scale <= 0.0f) {
          log.Log(kLogError, "scale_tolerance must be > 0.");
          valid_args = false;
        }
        i++;
      } else {
        valid_args = false;
      }

    } else if (arg == "-r" || arg == "--rotate") {
      if (i + 1 < argc - 1) {
        const float degrees = static_cast<float>(atof(argv[i + 1]));
        if (degrees <= 0.0f || degrees > 180.0f) {
          log.Log(kLogError, "rotate_tolerance must be >0 and <=180.");
          valid_args = false;
        } else {
          args->tolerances.rotate =
              motive::Angle::FromDegrees(degrees).ToRadians();
          i++;
        }
      } else {
        valid_args = false;
      }

    } else if (arg == "-t" || arg == "--translate") {
      if (i + 1 < argc - 1) {
        args->tolerances.translate = static_cast<float>(atof(argv[i + 1]));
        if (args->tolerances.translate <= 0.0f) {
          log.Log(kLogError, "translate_tolerance must be > 0.");
          valid_args = false;
        }
        i++;
      } else {
        valid_args = false;
      }

    } else if (arg == "-a" || arg == "--angle") {
      if (i + 1 < argc - 1) {
        const float degrees = static_cast<float>(atof(argv[i + 1]));
        if (degrees <= 0.0f || degrees > 90.0f) {
          log.Log(kLogError, "derivative_tolerance must be >0 and <=90.");
          valid_args = false;
        } else {
          args->tolerances.derivative_angle =
              motive::Angle::FromDegrees(degrees).ToRadians();
        }
        i++;
      } else {
        valid_args = false;
      }

    } else if (arg == "--repeat" || arg == "--norepeat") {
      const RepeatPreference repeat_preference =
          arg == "--repeat" ? kAlwaysRepeat : kNeverRepeat;
      if (args->repeat_preference != kRepeatIfRepeatable &&
          args->repeat_preference != repeat_preference) {
        log.Log(kLogError,
                "Only one of --repeat and --norepeat can be specified.\n");
        valid_args = false;
      } else {
        args->repeat_preference = repeat_preference;
      }

    } else if (arg == "--debug_time") {
      if (i + 1 < argc - 1) {
        args->debug_time = atoi(argv[i + 1]);
        args->log_level = kLogInfo;
        if (args->debug_time < 0) {
          log.Log(kLogError, "debug time must be >0.");
          valid_args = false;
        }
        i++;
      } else {
        valid_args = false;
      }

    } else {
      log.Log(kLogError, "Unknown parameter: %s\n", arg.c_str());
      valid_args = false;
    }
  }

  // Print usage.
  if (!valid_args) {
    log.Log(
        kLogImportant,
        "Usage: anim_pipeline [-v|-d|-i] [-o OUTPUT_FILE]\n"
        "           [-s SCALE_TOLERANCE] [-r ROTATE_TOLERANCE]\n"
        "           [-t TRANSLATE_TOLERANCE] [-a DERIVATIVE_TOLERANCE]\n"
        "           [--repeat|--norepeat] [--debug_time TIME] FBX_FILE\n"
        "\n"
        "Pipeline to convert FBX animations into FlatBuffer animations.\n"
        "Outputs a .motiveanim file with the same base name as FBX_FILE.\n\n"
        "Options:\n"
        "  -v, --verbose         output all informative messages\n"
        "  -d, --details         output important informative messages\n"
        "  -i, --info            output more than details, less than verbose.\n"
        "  -o, --out OUTPUT_FILE file to write .motiveanim file to;\n"
        "                        can be an absolute or relative path;\n"
        "                        when unspecified, uses base FBX name\n"
        "                        + .motiveanim\n"
        "  -s, --scale SCALE_TOLERANCE\n"
        "                        max deviation of output scale curves\n"
        "                        from input scale curves, unitless\n"
        "  -r, --rotate ROTATE_TOLERANCE\n"
        "                        max deviation of output rotate curves\n"
        "                        from intput rotate curves, in degrees\n"
        "  -t, --translate TRANSLATE_TOLERANCE\n"
        "                        max deviation of output translate curves\n"
        "                        from input translate curves, in scene's\n"
        "                        units of distance\n"
        "  -a, --angle DERIVATIVE_TOLERANCE\n"
        "                        max deviation of curve derivatives,\n"
        "                        considered as an angle in the x/y plane\n"
        "                        (e.g. derivative 1 ==> 45 degrees),\n"
        "                        in degrees.\n"
        "  --repeat, --norepeat  mark the animation as repeating or not\n"
        "                        repeating. A repeating animation cycles\n"
        "                        over and over. If neither option is\n"
        "                        specified, the animation is marked as\n"
        "                        repeating when it starts and ends\n"
        "                        with the same pose and derivatives.\n"
        "  --debug_time TIME     output the local transforms for each bone in\n"
        "                        the animation at TIME, in ms, and then exit.\n"
        "                        Useful for debugging situations where the\n"
        "                        runtime doesn't match source data.\n");
  }

  return valid_args;
}

}  // namespace motive

int main(int argc, char** argv) {
  motive::Logger log;

  // Parse the command line arguments.
  motive::AnimPipelineArgs args;
  if (!ParseAnimPipelineArgs(argc, argv, log, &args)) return 1;

  // Update the amount of information we're dumping.
  log.set_level(args.log_level);

  // Load the FBX file.
  motive::FbxAnimParser pipe(log);
  const bool load_status = pipe.Load(args.fbx_file.c_str());
  if (!load_status) return 1;

  // Output debug information for the specific time of the animation.
  if (args.debug_time >= 0) {
    pipe.LogAnimStateAtTime(args.debug_time);
    return 0;
  }

  // Gather data into a format conducive to our FlatBuffer format.
  motive::FlatAnim anim(args.tolerances, log);
  pipe.GatherFlatAnim(&anim);

  // Output gathered data to a binary FlatBuffer.
  anim.LogAllChannels();
  const bool output_status =
      anim.OutputFlatBuffer(args.output_file, args.repeat_preference);
  if (!output_status) return 1;

  // Success.
  return 0;
}
