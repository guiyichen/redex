/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeReference.h"

#include "MethodReference.h"
#include "Resolver.h"
#include "VirtualScope.h"
#include "Walkers.h"

namespace {

void fix_colliding_method(
    const Scope& scope,
    const std::map<DexMethod*, DexProto*, dexmethods_comparator>&
        colliding_methods) {
  // Fix colliding methods by appending an additional param.
  TRACE(REFU, 9, "sig: colliding_methods %d\n", colliding_methods.size());
  std::unordered_map<DexMethod*, size_t> num_additional_args;
  for (auto it : colliding_methods) {
    auto meth = it.first;
    auto new_proto = it.second;
    auto new_arg_list =
        type_reference::append_and_make(new_proto->get_args(), get_int_type());
    new_proto = DexProto::make_proto(new_proto->get_rtype(), new_arg_list);
    size_t arg_count = 1;
    while (DexMethod::get_method(
               meth->get_class(), meth->get_name(), new_proto) != nullptr) {
      new_arg_list = type_reference::append_and_make(new_proto->get_args(),
                                                     get_int_type());
      new_proto = DexProto::make_proto(new_proto->get_rtype(), new_arg_list);
      ++arg_count;
    }

    DexMethodSpec spec;
    spec.proto = new_proto;
    meth->change(spec,
                 false /* rename on collision */,
                 true /* update deobfuscated name */);
    num_additional_args[meth] = arg_count;

    auto code = meth->get_code();
    for (size_t i = 0; i < arg_count; ++i) {
      auto new_param_reg = code->allocate_temp();
      auto params = code->get_param_instructions();
      auto new_param_load = new IRInstruction(IOPCODE_LOAD_PARAM);
      new_param_load->set_dest(new_param_reg);
      code->insert_before(params.end(), new_param_load);
    }
    TRACE(REFU,
          9,
          "sig: patching colliding method %s with %d additional args\n",
          SHOW(meth),
          arg_count);
  }

  walk::parallel::code(scope, [&](DexMethod* meth, IRCode& code) {
    method_reference::CallSites callsites;
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (!insn->has_method()) {
        continue;
      }
      const auto callee =
          resolve_method(insn->get_method(),
                         opcode_to_search(const_cast<IRInstruction*>(insn)));
      if (callee == nullptr ||
          colliding_methods.find(callee) == colliding_methods.end()) {
        continue;
      }
      callsites.emplace_back(meth, &mie, callee);
    }

    for (const auto& callsite : callsites) {
      auto callee = callsite.callee;
      always_assert(callee != nullptr);
      TRACE(REFU,
            9,
            "sig: patching colliding method callsite to %s in %s\n",
            SHOW(callee),
            SHOW(meth));
      // 42 is a dummy int val as the additional argument to the patched
      // colliding method.
      std::vector<uint32_t> additional_args;
      for (size_t i = 0; i < num_additional_args.at(callee); ++i) {
        additional_args.push_back(42);
      }
      method_reference::NewCallee new_callee(callee, additional_args);
      method_reference::patch_callsite(callsite, new_callee);
    }
  });
}

} // namespace

namespace type_reference {

std::string get_method_signature(const DexMethod* method) {
  std::ostringstream ss;
  auto proto = method->get_proto();
  ss << show(proto->get_rtype()) << " ";
  ss << method->get_simple_deobfuscated_name();
  auto arg_list = proto->get_args();
  if (arg_list->size() > 0) {
    ss << "(";
    auto que = arg_list->get_type_list();
    for (auto t : que) {
      ss << show(t) << ", ";
    }
    ss.seekp(-2, std::ios_base::end);
    ss << ")";
  }

  return ss.str();
}

bool proto_has_reference_to(const DexProto* proto, const TypeSet& targets) {
  auto rtype = get_array_type_or_self(proto->get_rtype());
  if (targets.count(rtype) > 0) {
    return true;
  }
  std::deque<DexType*> lst;
  for (const auto arg_type : proto->get_args()->get_type_list()) {
    auto extracted_arg_type = get_array_type_or_self(arg_type);
    if (targets.count(extracted_arg_type) > 0) {
      return true;
    }
  }
  return false;
}

DexProto* update_proto_reference(
    const DexProto* proto,
    const std::unordered_map<const DexType*, DexType*>& old_to_new) {
  auto rtype = get_array_type_or_self(proto->get_rtype());
  if (old_to_new.count(rtype) > 0) {
    auto merger_type = old_to_new.at(rtype);
    auto level = get_array_level(proto->get_rtype());
    rtype = make_array_type(merger_type, level);
  } else {
    rtype = proto->get_rtype();
  }
  std::deque<DexType*> lst;
  for (const auto arg_type : proto->get_args()->get_type_list()) {
    auto extracted_arg_type = get_array_type_or_self(arg_type);
    if (old_to_new.count(extracted_arg_type) > 0) {
      auto merger_type = old_to_new.at(extracted_arg_type);
      auto level = get_array_level(arg_type);
      auto new_arg_type = make_array_type(merger_type, level);
      lst.push_back(new_arg_type);
    } else {
      lst.push_back(arg_type);
    }
  }

  return DexProto::make_proto(const_cast<DexType*>(rtype),
                              DexTypeList::make_type_list(std::move(lst)));
}

DexTypeList* prepend_and_make(const DexTypeList* list, DexType* new_type) {
  auto old_list = list->get_type_list();
  auto prepended = std::deque<DexType*>(old_list.begin(), old_list.end());
  prepended.push_front(new_type);
  return DexTypeList::make_type_list(std::move(prepended));
}

DexTypeList* append_and_make(const DexTypeList* list, DexType* new_type) {
  auto old_list = list->get_type_list();
  auto appended = std::deque<DexType*>(old_list.begin(), old_list.end());
  appended.push_back(new_type);
  return DexTypeList::make_type_list(std::move(appended));
}

DexTypeList* append_and_make(const DexTypeList* list,
                             const std::vector<DexType*>& new_types) {
  auto old_list = list->get_type_list();
  auto appended = std::deque<DexType*>(old_list.begin(), old_list.end());
  appended.insert(appended.end(), new_types.begin(), new_types.end());
  return DexTypeList::make_type_list(std::move(appended));
}

DexTypeList* replace_head_and_make(const DexTypeList* list, DexType* new_head) {
  auto old_list = list->get_type_list();
  auto new_list = std::deque<DexType*>(old_list.begin(), old_list.end());
  always_assert(!new_list.empty());
  new_list.pop_front();
  new_list.push_front(new_head);
  return DexTypeList::make_type_list(std::move(new_list));
}

DexTypeList* drop_and_make(const DexTypeList* list, size_t num_types_to_drop) {
  auto old_list = list->get_type_list();
  auto dropped = std::deque<DexType*>(old_list.begin(), old_list.end());
  for (size_t i = 0; i < num_types_to_drop; ++i) {
    dropped.pop_back();
  }
  return DexTypeList::make_type_list(std::move(dropped));
}

void update_method_signature_type_references(
    const Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& old_to_new,
    boost::optional<std::unordered_map<DexMethod*, std::string>&>
        method_debug_map) {
  std::map<DexMethod*, DexProto*, dexmethods_comparator> colliding_candidates;
  TypeSet mergeables;
  for (const auto& pair : old_to_new) {
    mergeables.insert(pair.first);
  }

  // 1. Updating the signature for non-ctor dmethods. Renaming is sufficient to
  // address method collision here. At the same time, we collect potentially
  // colliding methods that are more complicated to handle.
  const auto update_sig_simple = [&](DexMethod* meth) {
    auto proto = meth->get_proto();
    if (!proto_has_reference_to(proto, mergeables)) {
      return;
    }
    if (method_debug_map != boost::none) {
      method_debug_map.get()[meth] = get_method_signature(meth);
    }
    auto new_proto = update_proto_reference(proto, old_to_new);
    DexMethodSpec spec;
    spec.proto = new_proto;
    if (!is_init(meth) && !meth->is_virtual()) {
      TRACE(REFU, 9, "sig: updating non ctor/virt method %s\n", SHOW(meth));
      meth->change(spec,
                   true /* rename on collision */,
                   true /* update deobfuscated name */);
      return;
    }

    auto type = meth->get_class();
    auto name = meth->get_name();
    auto existing_meth = DexMethod::get_method(type, name, new_proto);
    if (existing_meth == nullptr) {
      // For ctors if no collision is found, we still perform the update.
      // This is needed to detect a subsequent collision on the updated ctor
      // proto.
      if (is_init(meth)) {
        TRACE(REFU, 9, "sig: updating ctor %s\n", SHOW(meth));
        meth->change(spec,
                     false /* rename on collision */,
                     true /* update deobfuscated name */);
      }
      return;
    }

    TRACE(REFU,
          9,
          "sig: found colliding candidate %s with %s\n",
          SHOW(existing_meth),
          SHOW(meth));
    colliding_candidates[meth] = new_proto;
  };

  walk::methods(scope, update_sig_simple);

  // 2. Updating non-colliding virtuals and ctors.
  // Updating them do not require handling of virtual scopes.
  const auto update_sig_non_colliding = [&](DexMethod* meth) {
    // Skip non-ctor dmethods since they are already updated.
    if (!is_init(meth) && !meth->is_virtual()) {
      return;
    }
    // Skip colliding candidates. We cannot simply update them. We need to
    // go through more complicated steps below.
    if (colliding_candidates.count(meth) > 0) {
      return;
    }
    auto proto = meth->get_proto();
    if (!proto_has_reference_to(proto, mergeables)) {
      return;
    }
    if (method_debug_map != boost::none) {
      method_debug_map.get()[meth] = get_method_signature(meth);
    }
    auto new_proto = update_proto_reference(proto, old_to_new);
    DexMethodSpec spec;
    spec.proto = new_proto;

    auto type = meth->get_class();
    auto name = meth->get_name();
    TRACE(REFU, 9, "sig: updating method %s\n", SHOW(meth));
    meth->change(spec,
                 true /* rename on collision */,
                 true /* update deobfuscated name */);
  };

  if (colliding_candidates.empty()) {
    walk::parallel::methods(scope, update_sig_non_colliding);
    return;
  }

  // If we found colliding candidates, we need to check their true virtualness
  // by computing virtual scopes of the current type system. We need to do that
  // before updating any virtual methods. Because updating virtual method will
  // break the existing virtual scopes.
  TRACE(REFU, 9, "sig: fixing colliding candidates\n");
  auto non_virtuals = devirtualize(scope);
  std::unordered_set<DexMethod*> non_virt_set(non_virtuals.begin(),
                                              non_virtuals.end());

  // 3. Updating non-ctor dmethods and non-colliding vmethods first.
  walk::parallel::methods(scope, update_sig_non_colliding);

  std::map<DexMethod*, DexProto*, dexmethods_comparator> colliding_methods;
  for (const auto& pair : colliding_candidates) {
    auto meth = pair.first;
    auto new_proto = pair.second;
    if (non_virt_set.count(meth) > 0) {
      DexMethodSpec spec;
      spec.proto = new_proto;
      TRACE(REFU, 9, "sig: updating non virt method %s\n", SHOW(meth));
      meth->change(spec,
                   true /* rename on collision */,
                   true /* update deobfuscated name */);
      continue;
    }
    // 4. Break the build if we have to handle the renaming of true virtuals.
    always_assert_log(!meth->is_virtual(),
                      "sig: found true virtual colliding method %s\n",
                      SHOW(meth));
    colliding_methods[meth] = new_proto;
  }

  // 5. Last resort. Fix colliding methods for non-virtuals.
  fix_colliding_method(scope, colliding_methods);
}

void update_field_type_references(
    const Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& old_to_new) {
  TRACE(REFU, 4, " updating field refs\n");
  const auto update_fields = [&](const std::vector<DexField*>& fields) {
    for (const auto field : fields) {
      const auto ref_type = field->get_type();
      const auto type = get_array_type_or_self(ref_type);
      if (old_to_new.count(type) == 0) {
        continue;
      }
      DexFieldSpec spec;
      auto new_type = old_to_new.at(type);
      auto level = get_array_level(ref_type);
      auto new_type_incl_array = make_array_type(new_type, level);
      spec.type = new_type_incl_array;
      field->change(spec);
      TRACE(REFU, 9, " updating field ref to %s\n", SHOW(type));
    }
  };

  for (const auto cls : scope) {
    update_fields(cls->get_ifields());
    update_fields(cls->get_sfields());
  }
}

} // namespace type_reference
