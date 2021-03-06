// Copyright (c) 2016 Google Inc.
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

#include "set_spec_constant_default_value_pass.h"

#include <cstring>
#include <cctype>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "spirv-tools/libspirv.h"
#include "util/parse_number.h"

#include "def_use_manager.h"
#include "make_unique.h"
#include "type_manager.h"
#include "types.h"

namespace spvtools {
namespace opt {

namespace {
using spvutils::NumberType;
using spvutils::EncodeNumberStatus;
using spvutils::ParseNumber;
using spvutils::ParseAndEncodeNumber;

// Given a numeric value in a null-terminated c string and the expected type of
// the value, parses the string and encodes it in a vector of words. If the
// value is a scalar integer or floating point value, encodes the value in
// SPIR-V encoding format. If the value is 'false' or 'true', returns a vector
// with single word with value 0 or 1 respectively. Returns the vector
// containing the encoded value on success. Otherwise returns an empty vector.
std::vector<uint32_t> ParseDefaultValueStr(const char* text,
                                           const analysis::Type* type) {
  std::vector<uint32_t> result;
  if (!strcmp(text, "true")) {
    result.push_back(1u);
  } else if (!strcmp(text, "false")) {
    result.push_back(0u);
  } else {
    NumberType number_type = {32, SPV_NUMBER_UNSIGNED_INT};
    if (const auto* IT = type->AsInteger()) {
      number_type.bitwidth = IT->width();
      number_type.kind =
          IT->IsSigned() ? SPV_NUMBER_SIGNED_INT : SPV_NUMBER_UNSIGNED_INT;
    } else if (const auto* FT = type->AsFloat()) {
      number_type.bitwidth = FT->width();
      number_type.kind = SPV_NUMBER_FLOATING;
    }
    EncodeNumberStatus rc = ParseAndEncodeNumber(
        text, number_type, [&result](uint32_t word) { result.push_back(word); },
        nullptr);
    // Clear the result vector on failure.
    if (rc != EncodeNumberStatus::kSuccess) {
      result.clear();
    }
  }
  return result;
}

// Returns true if the given instruction's result id could have a SpecId
// decoration.
bool CanHaveSpecIdDecoration(const ir::Instruction& inst) {
  switch (inst.opcode()) {
    case SpvOp::SpvOpSpecConstant:
    case SpvOp::SpvOpSpecConstantFalse:
    case SpvOp::SpvOpSpecConstantTrue:
      return true;
    default:
      return false;
  }
}

// Given a decoration group defining instruction that is decorated with SpecId
// decoration, finds the spec constant defining instruction which is the real
// target of the SpecId decoration. Returns the spec constant defining
// instruction if such an instruction is found, otherwise returns a nullptr.
ir::Instruction* GetSpecIdTargetFromDecorationGroup(
    const ir::Instruction& decoration_group_defining_inst,
    analysis::DefUseManager* def_use_mgr) {
  // Find the OpGroupDecorate instruction which consumes the given decoration
  // group. Note that the given decoration group has SpecId decoration, which
  // is unique for different spec constants. So the decoration group cannot be
  // consumed by different OpGroupDecorate instructions. Therefore we only need
  // the first OpGroupDecoration instruction that uses the given decoration
  // group.
  ir::Instruction* group_decorate_inst = nullptr;
  for (const auto& u :
       *def_use_mgr->GetUses(decoration_group_defining_inst.result_id())) {
    if (u.inst->opcode() == SpvOp::SpvOpGroupDecorate) {
      group_decorate_inst = u.inst;
      break;
    }
  }
  if (!group_decorate_inst) return nullptr;

  // Scan through the target ids of the OpGroupDecorate instruction. There
  // should be only one spec constant target consumes the SpecId decoration.
  // If multiple target ids are presented in the OpGroupDecorate instruction,
  // they must be the same one that defined by an eligible spec constant
  // instruction. If the OpGroupDecorate instruction has different target ids
  // or a target id is not defined by an eligible spec cosntant instruction,
  // returns a nullptr.
  ir::Instruction* target_inst = nullptr;
  for (uint32_t i = 1; i < group_decorate_inst->NumInOperands(); i++) {
    // All the operands of a OpGroupDecorate instruction should be of type
    // SPV_OPERAND_TYPE_ID.
    uint32_t candidate_id = group_decorate_inst->GetSingleWordInOperand(i);
    ir::Instruction* candidate_inst = def_use_mgr->GetDef(candidate_id);

    if (!candidate_inst) {
      continue;
    }

    if (!target_inst) {
      // If the spec constant target has not been found yet, check if the
      // candidate instruction is the target.
      if (CanHaveSpecIdDecoration(*candidate_inst)) {
        target_inst = candidate_inst;
      } else {
        // Spec id decoration should not be applied on other instructions.
        // TODO(qining): Emit an error message in the invalid case once the
        // error handling is done.
        return nullptr;
      }
    } else {
      // If the spec constant target has been found, check if the candidate
      // instruction is the same one as the target. The module is invalid if
      // the candidate instruction is different with the found target.
      // TODO(qining): Emit an error messaage in the invalid case once the
      // error handling is done.
      if (candidate_inst != target_inst) return nullptr;
    }
  }
  return target_inst;
}
};

bool SetSpecConstantDefaultValuePass::Process(ir::Module* module) {
  // The operand index of decoration target in an OpDecorate instruction.
  const uint32_t kTargetIdOperandIndex = 0;
  // The operand index of the decoration literal in an OpDecorate instruction.
  const uint32_t kDecorationOperandIndex = 1;
  // The operand index of Spec id literal value in an OpDecorate SpecId
  // instruction.
  const uint32_t kSpecIdLiteralOperandIndex = 2;
  // The number of operands in an OpDecorate SpecId instruction.
  const uint32_t kOpDecorateSpecIdNumOperands = 3;
  // The in-operand index of the default value in a OpSpecConstant instruction.
  const uint32_t kOpSpecConstantLiteralInOperandIndex = 0;

  bool modified = false;
  analysis::DefUseManager def_use_mgr(module);
  analysis::TypeManager type_mgr(*module);
  // Scan through all the annotation instructions to find 'OpDecorate SpecId'
  // instructions. Then extract the decoration target of those instructions.
  // The decoration targets should be spec constant defining instructions with
  // opcode: OpSpecConstant{|True|False}. The spec id of those spec constants
  // will be used to look up their new default values in the mapping from
  // spec id to new default value strings. Once a new default value string
  // is found for a spec id, the string will be parsed according to the target
  // spec constant type. The parsed value will be used to replace the original
  // default value of the target spec constant.
  for (ir::Instruction& inst : module->annotations()) {
    // Only process 'OpDecorate SpecId' instructions
    if (inst.opcode() != SpvOp::SpvOpDecorate) continue;
    if (inst.NumOperands() != kOpDecorateSpecIdNumOperands) continue;
    if (inst.GetSingleWordInOperand(kDecorationOperandIndex) !=
        uint32_t(SpvDecoration::SpvDecorationSpecId)) {
      continue;
    }

    // 'inst' is an OpDecorate SpecId instruction.
    uint32_t spec_id = inst.GetSingleWordOperand(kSpecIdLiteralOperandIndex);
    uint32_t target_id = inst.GetSingleWordOperand(kTargetIdOperandIndex);

    // Find the spec constant defining instruction. Note that the
    // target_id might be a decoration group id.
    ir::Instruction* spec_inst = nullptr;
    if (ir::Instruction* target_inst = def_use_mgr.GetDef(target_id)) {
      if (target_inst->opcode() == SpvOp::SpvOpDecorationGroup) {
        spec_inst =
            GetSpecIdTargetFromDecorationGroup(*target_inst, &def_use_mgr);
      } else {
        spec_inst = target_inst;
      }
    } else {
      continue;
    }
    if (!spec_inst) continue;

    // Search for the new default value for this spec id.
    auto iter = spec_id_to_value_.find(spec_id);
    if (iter == spec_id_to_value_.end()) continue;

    // Gets the string of the default value and parses it to bit pattern
    // with the type of the spec constant.
    const std::string& default_value_str = iter->second;
    std::vector<uint32_t> bit_pattern = ParseDefaultValueStr(
        default_value_str.c_str(), type_mgr.GetType(spec_inst->type_id()));
    if (bit_pattern.empty()) continue;

    // Update the operand bit patterns of the spec constant defining
    // instruction.
    switch (spec_inst->opcode()) {
      case SpvOp::SpvOpSpecConstant:
        // If the new value is the same with the original value, no
        // need to do anything. Otherwise update the operand words.
        if (spec_inst->GetInOperand(kOpSpecConstantLiteralInOperandIndex)
                .words != bit_pattern) {
          spec_inst->SetInOperand(kOpSpecConstantLiteralInOperandIndex,
                                  std::move(bit_pattern));
          modified = true;
        }
        break;
      case SpvOp::SpvOpSpecConstantTrue:
        // If the new value is also 'true', no need to change anything.
        // Otherwise, set the opcode to OpSpecConstantFalse;
        if (!static_cast<bool>(bit_pattern.front())) {
          spec_inst->SetOpcode(SpvOp::SpvOpSpecConstantFalse);
          modified = true;
        }
        break;
      case SpvOp::SpvOpSpecConstantFalse:
        // If the new value is also 'false', no need to change anything.
        // Otherwise, set the opcode to OpSpecConstantTrue;
        if (static_cast<bool>(bit_pattern.front())) {
          spec_inst->SetOpcode(SpvOp::SpvOpSpecConstantTrue);
          modified = true;
        }
        break;
      default:
        break;
    }
    // No need to update the DefUse manager, as this pass does not change any
    // ids.
  }
  return modified;
}

// Returns true if the given char is ':', '\0' or considered as blank space
// (i.e.: '\n', '\r', '\v', '\t', '\f' and ' ').
bool IsSeparator(char ch) {
  return std::strchr(":\0", ch) || std::isspace(ch) != 0;
}

std::unique_ptr<SetSpecConstantDefaultValuePass::SpecIdToValueStrMap>
SetSpecConstantDefaultValuePass::ParseDefaultValuesString(const char* str) {
  if (!str) return nullptr;

  auto spec_id_to_value = MakeUnique<SpecIdToValueStrMap>();

  // The parsing loop, break when points to the end.
  while (*str) {
    // Find the spec id.
    while (std::isspace(*str)) str++;  // skip leading spaces.
    const char* entry_begin = str;
    while (!IsSeparator(*str)) str++;
    const char* entry_end = str;
    std::string spec_id_str(entry_begin, entry_end - entry_begin);
    uint32_t spec_id = 0;
    if (!ParseNumber(spec_id_str.c_str(), &spec_id)) {
      // The spec id is not a valid uint32 number.
      return nullptr;
    }
    auto iter = spec_id_to_value->find(spec_id);
    if (iter != spec_id_to_value->end()) {
      // Same spec id has been defined before
      return nullptr;
    }
    // Find the ':', spaces between the spec id and the ':' are not allowed.
    if (*str++ != ':') {
      // ':' not found
      return nullptr;
    }
    // Find the value string
    const char* val_begin = str;
    while (!IsSeparator(*str)) str++;
    const char* val_end = str;
    if (val_end == val_begin) {
      // Value string is empty.
      return nullptr;
    }
    // Update the mapping with spec id and value string.
    (*spec_id_to_value)[spec_id] = std::string(val_begin, val_end - val_begin);

    // Skip trailing spaces.
    while (std::isspace(*str)) str++;
  }

  return spec_id_to_value;
}

}  // namespace opt
}  // namespace spvtools
