/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexAnnotation.h"
#include "DexClass.h"

namespace api {

/**
 * This class exists to check the required Android API level for a method.
 *
 * The api level information is backed by
 *   DexMethod.rstate.[get_api_level|set_api_level].
 * This state is initialized for all initially loaded classes, and computed
 * lazily on first access for all dynamically created method.
 * If desired at a point in time after annotations have been erased, this state
 * can be manually set by optimization passes that create methods.
 */
class LevelChecker {
 public:
  /**
   * Call `init` exactly once after the classes have been loaded from the dex
   * file. We have this initialization function (instead of a constructor)
   * because the static instance is created before the classes are loaded.
   *
   * `min_level` is the api level that un-annotated code should be assumed to
   * have.
   *
   * After this initialization, `get_method_level` can be called in parallel
   * safely.
   */
  static void init(int32_t min_level, const Scope& scope);

  /**
   * Get the "most specific" api level of this method. If the method is
   * annotated with Target/RequiresApi, return its level, if not, check the
   * containing class and return its level. If neither are annotated with
   * Target/RequiresApi, return s_min_level.
   * Annotations that have a value less than s_min_level are interpreted to mean
   * s_min_level.
   *
   * In an attempt to mimimize restrictions on optimizations, this method groups
   * inner classes into the api level of their outer class. This isn't strictly
   * correct but it seems like most developers just forgot (or were too lazy) to
   * put the annotation on all the inner classes (especially anonymous ones).
   */
  static int32_t get_method_level(DexMethod* method);

  /**
   * Return the minimum api level of the entire app. This is the lowest value
   * that `get_method_level` or `get_level` could return. Members with no
   * annotations have this api level.
   */
  static int32_t get_min_level();

  /**
   * Only Check the annotations of the given member, not any containing class
   * levels. If this member is not annotated, return -1.
   * Annotations that have a value less than s_min_level are interpreted to mean
   * s_min_level.
   */
  template <typename DexMember>
  static int32_t get_level(const DexMember* member) {
    if ((s_requires_api == nullptr && s_target_api == nullptr) ||
        member->get_anno_set() == nullptr) {
      return -1;
    }
    for (const auto& anno : member->get_anno_set()->get_annotations()) {
      if (anno->type() != nullptr &&
          (anno->type() == s_requires_api || anno->type() == s_target_api)) {
        const auto& elems = anno->anno_elems();
        always_assert(elems.size() == 1);
        const DexAnnotationElement& api_elem = elems[0];
        always_assert(api_elem.string == DexString::get_string("api") ||
                      api_elem.string == DexString::get_string("value"));
        const DexEncodedValue* value = api_elem.encoded_value;
        always_assert(value->evtype() == DEVT_INT);
        int32_t result = static_cast<int32_t>(value->value());
        return std::max(result, s_min_level);
      }
    }
    return -1;
  }

 private:
  static DexClass* get_outer_class(const DexClass* cls);
  static void init_class(DexClass* clazz);
  static void init_method(DexMethod* method);

  /**
   * These states are only meant to be edited during `init`. After
   * initialization, these are read-only and safe to use in parallel
   */
  static int32_t s_min_level;
  static DexType* s_requires_api;
  static DexType* s_target_api;
  static bool s_has_been_init;
};

} // namespace api
