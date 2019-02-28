/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ProguardMap.h"

#include "DexUtil.h"
#include "Timer.h"
#include "WorkQueue.h"

namespace {

std::string find_or_same(
    const std::string& key,
    const std::unordered_map<std::string, std::string>& map) {
  auto it = map.find(key);
  if (it == map.end()) return key;
  return it->second;
}

std::string convert_scalar_type(std::string type) {
  static const std::unordered_map<std::string, std::string> prim_map =
    {{"void",    "V"},
     {"boolean", "Z"},
     {"byte",    "B"},
     {"short",   "S"},
     {"char",    "C"},
     {"int",     "I"},
     {"long",    "J"},
     {"float",   "F"},
     {"double",  "D"}};
  auto it = prim_map.find(type);
  if (it != prim_map.end()) {
    return it->second;
  }
  return JavaNameUtil::external_to_internal(type);
}

std::string convert_field(const std::string &cls,
    const std::string &type,
    const std::string &name) {
  std::ostringstream ss;
  ss << cls << "." << name << ":" << type;
  return ss.str();
}

std::string convert_method(
  const std::string &cls,
  const std::string &rtype,
  const std::string &methodname,
  const std::string &args
) {
  std::ostringstream ss;
  ss << cls << "." << methodname << ":(" << args << ")" << rtype;
  return ss.str();
}

std::string translate_type(const std::string& type, const ProguardMap& pm) {
  auto base_start = type.find_first_not_of("[");
  auto array_prefix = type.substr(0, base_start);
  auto base_type = type.substr(base_start);
  array_prefix += pm.translate_class(base_type);
  return array_prefix;
}

void whitespace(const char*& p) {
  while (isspace(*p)) {
    ++p;
  }
}

uint32_t line_number(const char*& p) {
  char* e;
  uint32_t line = std::strtol(p, &e, 10);
  p = e;
  return line;
}

bool isseparator(uint32_t cp) {
  return cp == '\0' ||
    cp == ' ' ||
    cp == ':' ||
    cp == ',' ||
    cp == '\n' ||
    cp == '(' ||
    cp == ')';
}

bool id(const char*& p, std::string& s) {
  auto b = p;
  auto first = mutf8_next_code_point(p);
  if (isdigit(first)) return false;
  while (true) {
    auto prev = p;
    auto cp = mutf8_next_code_point(p);
    if (isseparator(cp)) {
      p = prev;
      s = std::string(b, p - b);
      return true;
    }
  }
}

bool literal(const char*& p, const char* s) {
  auto len = strlen(s);
  bool rv = !strncmp(p, s, len);
  p += len;
  return rv;
}

bool literal(const char*& p, char s) {
  if (*p == s) {
    ++p;
    return true;
  }
  return false;
}

bool comment(const std::string& line) {
  auto p = line.c_str();
  whitespace(p);
  return literal(p, '#');
}

void inlined_method(std::string& classname, std::string& methodname) {
  std::size_t found = methodname.find_last_of('.');
  if (found != std::string::npos) {
    classname = convert_scalar_type(methodname.substr(0, found));
    methodname = methodname.substr(found + 1);
  }
}

/**
 * Proguard would generate some special sequences when a coalesced interface is used.
 * https://sourceforge.net/p/proguard/code/ci/default/tree/core/src/proguard/classfile/editor/ClassReferenceFixer.java#l554
 * Before:
 *   com.facebook.imagepipeline.core.ExecutorSupplier mExecutorSupplier;
 * After:
 *   a_vcard.android.syncml.pim.VBuilder mExecutorSupplier$7ec36e13 -> b
 */
bool is_maybe_proguard_generated_member(const std::string& s) {
  unsigned int count = 0;
  for (auto it = s.rbegin(); it != s.rend(); ++it, ++count) {
    if (isxdigit(*it)) continue;
    if (*it == '$') return count == 8;
    return false;
  }
  return false;
}
} // namespace

ProguardMap::ProguardMap(const std::string& filename) {
  if (!filename.empty()) {
    Timer t("Parsing proguard map");
    std::ifstream fp(filename);
    always_assert_log(fp, "Can't open proguard map: %s\n", filename.c_str());
    parse_proguard_map(fp);
  }
}

std::string ProguardMap::translate_class(const std::string& cls) const {
  return find_or_same(cls, m_classMap);
}

std::string ProguardMap::translate_field(const std::string& field) const {
  return find_or_same(field, m_fieldMap);
}

std::string ProguardMap::translate_method(const std::string& method) const {
  return find_or_same(method, m_methodMap);
}

std::string ProguardMap::deobfuscate_class(const std::string& cls) const {
  return find_or_same(cls, m_obfClassMap);
}

std::string ProguardMap::deobfuscate_field(const std::string& field) const {
  return find_or_same(field, m_obfFieldMap);
}

std::string ProguardMap::deobfuscate_method(const std::string& method) const {
  return find_or_same(method, m_obfMethodMap);
}

ProguardLineRangeVector& ProguardMap::method_lines(
    const std::string& obfuscated_method) {
  return m_obfMethodLinesMap.at(obfuscated_method);
}

void ProguardMap::parse_proguard_map(std::istream& fp) {
  std::string line;
  while (std::getline(fp, line)) {
    parse_class(line);
  }
  fp.clear();
  fp.seekg(0);
  assert_log(!fp.fail(), "Can't use ProguardMap with non-seekable stream");
  while (std::getline(fp, line)) {
    if (parse_class(line)) {
      continue;
    }
    if (parse_field(line)) {
      continue;
    }
    if (parse_method(line)) {
      continue;
    }
    if (comment(line)) {
      continue;
    }
    always_assert_log(false,
                      "Bogus line encountered in proguard map: %s\n",
                      line.c_str());
  }
}

bool ProguardMap::parse_class(const std::string& line) {
  std::string classname;
  std::string newname;
  auto p = line.c_str();
  if (!id(p, classname)) return false;
  if (!literal(p, " -> ")) return false;
  if (!id(p, newname)) return false;
  m_currClass = convert_type(classname);
  m_currNewClass = convert_type(newname);
  m_classMap[m_currClass] = m_currNewClass;
  m_obfClassMap[m_currNewClass] = m_currClass;
  return true;
}

bool ProguardMap::parse_field(const std::string& line) {
  std::string type;
  std::string fieldname;
  std::string newname;

  auto p = line.c_str();
  whitespace(p);
  if (!id(p, type)) return false;
  whitespace(p);
  if (!id(p, fieldname)) return false;
  if (!literal(p, " -> ")) return false;
  if (!id(p, newname)) return false;

  auto ctype = convert_type(type);
  auto xtype = translate_type(ctype, *this);
  auto pgnew = convert_field(m_currNewClass, xtype, newname);
  auto pgold = convert_field(m_currClass, ctype, fieldname);
  // Record interfaces that are coalesced by Proguard.
  if (ctype[0] == 'L' && is_maybe_proguard_generated_member(fieldname)) {
    fprintf(stderr,
            "Type '%s' is touched by Proguard in '%s'\n",
            ctype.c_str(),
            pgold.c_str());
    m_pg_coalesced_interfaces.insert(ctype);
  }
  m_fieldMap[pgold] = pgnew;
  m_obfFieldMap[pgnew] = pgold;
  return true;
}

bool ProguardMap::parse_method(const std::string& line) {
  std::string type;
  std::string methodname;
  std::string classname = m_currClass;
  std::string old_args;
  std::string new_args;
  std::string newname;
  auto lines = std::make_unique<ProguardLineRange>();
  auto p = line.c_str();
  whitespace(p);
  lines->start = line_number(p);
  literal(p, ':');
  lines->end = line_number(p);
  literal(p, ':');

  if (!id(p, type)) return false;
  whitespace(p);

  if (!id(p, methodname)) return false;
  inlined_method(classname, methodname);

  if (!literal(p, '(')) return false;
  while (true) {
    std::string arg;
    if (literal(p, ')')) break;
    id(p, arg);
    auto old_arg = convert_type(arg);
    auto new_arg = translate_type(old_arg, *this);
    old_args += old_arg;
    new_args += new_arg;
    literal(p, ',');
  }

  literal(p, ':');
  lines->original_start = line_number(p);
  literal(p, ':');
  lines->original_end = line_number(p);
  literal(p, " -> ");

  if (!id(p, newname)) return false;

  auto old_rtype = convert_type(type);
  auto new_rtype = translate_type(old_rtype, *this);
  auto pgold = convert_method(classname, old_rtype, methodname, old_args);
  auto pgnew = convert_method(m_currNewClass, new_rtype, newname, new_args);
  m_methodMap[pgold] = pgnew;
  m_obfMethodMap[pgnew] = pgold;
  lines->original_name = pgold;
  m_obfMethodLinesMap[pgnew].push_back(std::move(lines));
  return true;
}

void apply_deobfuscated_names(const std::vector<DexClasses>& dexen,
                              const ProguardMap& pm) {
  std::function<void(DexClass*)> worker_empty_pg_map = [&](DexClass* cls) {
    cls->set_deobfuscated_name(show(cls));
    for (const auto& m : cls->get_dmethods()) {
      m->set_deobfuscated_name(show(m));
    }
    for (const auto& m : cls->get_vmethods()) {
      m->set_deobfuscated_name(show(m));
    }
    for (const auto& f : cls->get_ifields()) {
      f->set_deobfuscated_name(show(f));
    }
    for (const auto& f : cls->get_sfields()) {
      f->set_deobfuscated_name(show(f));
    }
  };

  std::function<void(DexClass*)> worker_pg_map = [&](DexClass* cls) {
    TRACE(PGR, 4, "deob cls %s %s\n", SHOW(cls),
          pm.deobfuscate_class(show(cls)).c_str());
    cls->set_deobfuscated_name(pm.deobfuscate_class(show(cls)));
    for (const auto& m : cls->get_dmethods()) {
      TRACE(PGR, 4, "deob dmeth %s %s\n", SHOW(m),
            pm.deobfuscate_method(show(m)).c_str());
      m->set_deobfuscated_name(pm.deobfuscate_method(show(m)));
    }
    for (const auto& m : cls->get_vmethods()) {
      TRACE(PM, 4, "deob vmeth %s %s\n", SHOW(m),
            pm.deobfuscate_method(show(m)).c_str());
      m->set_deobfuscated_name(pm.deobfuscate_method(show(m)));
    }
    for (const auto& f : cls->get_ifields()) {
      TRACE(PM, 4, "deob ifield %s %s\n", SHOW(f),
            pm.deobfuscate_field(show(f)).c_str());
      f->set_deobfuscated_name(pm.deobfuscate_field(show(f)));
    }
    for (const auto& f : cls->get_sfields()) {
      TRACE(PM, 4, "deob sfield %s %s\n", SHOW(f),
            pm.deobfuscate_field(show(f)).c_str());
      f->set_deobfuscated_name(pm.deobfuscate_field(show(f)));
    }
  };

  auto wq = workqueue_foreach<DexClass*>(
      pm.empty() ? worker_empty_pg_map : worker_pg_map);

  for (const auto& dex : dexen) {
    for (const auto& cls : dex) {
      wq.add_item(cls);
    }
  }

  wq.run_all();
}

std::string convert_type(std::string type) {
  auto dimpos = type.find('[');
  if (dimpos == std::string::npos) {
    return convert_scalar_type(type);
  }
  auto ndims = std::count(type.begin() + dimpos, type.end(), '[');
  std::string res(ndims, '[');
  return res + convert_scalar_type(type.substr(0, dimpos));
}
