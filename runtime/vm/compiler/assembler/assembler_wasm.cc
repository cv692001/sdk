// Copyright (c) 2020, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler/assembler/assembler_wasm.h"

#include "vm/log.h"

namespace wasm {
namespace {
using ::dart::FLAG_trace_wasm_compilation;  // For use in WasmTrace.
using ::dart::Log;                          // For use in WasmTrace.
using ::dart::OS;
using ::dart::ReAlloc;
using ::dart::SExpInteger;  // Uses int64_t internally, so it should fit most
                            // serialized Wasm integers.
using ::dart::SExpList;
using ::dart::SExpression;
using ::dart::SExpSymbol;
using ::dart::ValueObject;
using ::dart::WriteStream;
using ::dart::Zone;
}  // namespace

void WasmTrace(const char* format, ...) {
  if (FLAG_trace_wasm_compilation) {
    va_list args;
    va_start(args, format);
    THR_VPrint(format, args);
    va_end(args);
  }
}

// PushBytecountFrontScope is an instance of the RAII programming pattern.
// It is used in the WRITE_BYTECOUNT() macro, which is to only be used
// inside the Output methods of the Wasm classes (i.e. when a WriteStream*
// stream is available for use).
// Example usage and semantics:
//
//  The following *scoped* code inside an Output method:
//  {
//    WRITE_BYTECOUNT();
//    WRITE_UNSIGNED(23);
//    WRITE_UNSIGNED(98);
//  }
//
//  Has the same effect as the following:
//  {
//     WRITE_UNSIGNED(2);  // Since 2 bytes follow in the enclosing scope.
//     WRITE_UNSIGNED(23);
//     WRITE_UNSIGNED(98);
//  }
//
//  Both code fragments write the list [2, 23, 98].
class PushBytecountFrontScope : public ValueObject {
 public:
  // The current WriteStream* is replaced by new stream (and internal buffer)
  // each time the WRITE_BYTECOUNT() macro occurs in a scope. Then,
  // upon leaving the scope, the old stream (and bufer) is restored, with
  // the new contents and their bytecount appended (the latter coming first).
  explicit PushBytecountFrontScope(WriteStream** current_stream_location)
      : replacement_buffer_(nullptr),
        old_stream_location_(current_stream_location),
        old_stream_(*current_stream_location),
        new_stream_(&replacement_buffer_, old_stream_->alloc(), 16) {
    *current_stream_location = &new_stream_;
  }

  ~PushBytecountFrontScope() {
    const intptr_t bytes_written = new_stream_.bytes_written();
    WasmTrace("Copying %" Pd " bytes to parent WriteStream.\n", bytes_written);
    // The Wasm specification treats bytes_written as an unsigned 32 bit integer.
    // While an overflow is technically possible, code of this size is unlikely
    // to occur in practice.
    old_stream_->WriteUnsigned(bytes_written);
    old_stream_->WriteBytes(replacement_buffer_, bytes_written);
    *old_stream_location_ = old_stream_;
  }

 private:
  uint8_t* replacement_buffer_;
  WriteStream** const old_stream_location_;
  WriteStream* const old_stream_;
  WriteStream new_stream_;

  DISALLOW_COPY_AND_ASSIGN(PushBytecountFrontScope);
};

#define WRITE_BYTE(m) stream->WriteFixed(static_cast<uint8_t>(m))
#define WRITE_UNSIGNED(m) stream->WriteUnsigned(m)
#define WRITE_SIGNED(m) stream->Write(m)

// It is not permitted to have two occurences of this macro in the same
// enclosing scope. This is enforced by a variable name clash if this is
// not respected. This requires users to explicitly open a new scope whenever
// they need multiply-nested PushBytecountFrontScopes, consistent with the
// example uses above.
#define WRITE_BYTECOUNT()                                                      \
  PushBytecountFrontScope push_bytecount_front_scope(&stream)

SExpression* NumType::Serialize(Zone* zone) {
  switch (kind_) {
    case Kind::kI32:
      return new (zone) SExpSymbol("i32");
    case Kind::kI64:
      return new (zone) SExpSymbol("i64");
    case Kind::kF32:
      return new (zone) SExpSymbol("f32");
    case Kind::kF64:
      return new (zone) SExpSymbol("f64");
    case Kind::kV128:
      return new (zone) SExpSymbol("v128");
    default:
      UNREACHABLE();
  }
}

void NumType::OutputBinary(WriteStream* stream) {
  switch (kind_) {
    case Kind::kI32:
      WRITE_BYTE(0x7F);
      break;
    case Kind::kI64:
      WRITE_BYTE(0x7E);
      break;
    case Kind::kF32:
      WRITE_BYTE(0x7D);
      break;
    case Kind::kF64:
      WRITE_BYTE(0x7C);
      break;
    case Kind::kV128:
      WRITE_BYTE(0x7B);
      break;
    default:
      UNREACHABLE();
  }
}

SExpression* RefType::Serialize(Zone* zone) {
  ASSERT(heap_type_ != nullptr);
  // First, try to use the shorthand notations.
  if (!nullable_ && heap_type_->kind_ == HeapType::Kind::kI31) {
    // ref i31 = i31ref.
    return new (zone) SExpSymbol("i31ref");
  } else if (nullable_) {
    // ref null {func/extern/any/eq} = {func/extern/any/eq}ref.
    switch (heap_type_->kind_) {
      case HeapType::Kind::kFunc:
        return new (zone) SExpSymbol("funcref");
      case HeapType::Kind::kExtern:
        return new (zone) SExpSymbol("externref");
      case HeapType::Kind::kAny:
        return new (zone) SExpSymbol("anyref");
      case HeapType::Kind::kEq:
        return new (zone) SExpSymbol("eqref");
      default:
        break;
    }
  }
  // Otherwise, use the general case.
  const auto sexp = new (zone) SExpList(zone);
  sexp->Add(new (zone) SExpSymbol("ref"));
  if (nullable_) {
    sexp->Add(new (zone) SExpSymbol("null"));
  }
  sexp->Add(heap_type_->Serialize(zone));
  return sexp;
}

void RefType::OutputBinary(WriteStream* stream) {
  ASSERT(heap_type_ != nullptr);
  // First, try to use the shorthand notations.
  if (!nullable_ && heap_type_->kind_ == HeapType::Kind::kI31) {
    // ref i31 = i31ref.
    WRITE_SIGNED(-0x16);
    return;
  } else if (nullable_) {
    // ref null {func/extern/any/eq} = {func/extern/any/eq}ref.
    switch (heap_type_->kind_) {
      case HeapType::Kind::kFunc:
      case HeapType::Kind::kExtern:
      case HeapType::Kind::kAny:
      case HeapType::Kind::kEq:
        heap_type_->OutputBinary(stream);
        return;
      default:
        break;
    }
  }
  // Otherwise, use the general case.
  if (nullable_) {
    WRITE_SIGNED(-0x14);
  } else {
    WRITE_SIGNED(-0x15);
  }
  heap_type_->OutputBinary(stream);
}

SExpression* HeapType::Serialize(Zone* zone) {
  switch (kind_) {
    case Kind::kFunc:
      return new (zone) SExpSymbol("func");
    case Kind::kExtern:
      return new (zone) SExpSymbol("extern");
    case Kind::kTypeidx:
      return new (zone) SExpInteger(def_type_->index());
    case Kind::kAny:
      return new (zone) SExpSymbol("any");
    case Kind::kEq:
      return new (zone) SExpSymbol("eq");
    case Kind::kI31:
      return new (zone) SExpSymbol("i31");
    default:
      UNREACHABLE();
  }
}

void HeapType::OutputBinary(WriteStream* stream) {
  switch (kind_) {
    case Kind::kFunc:
      WRITE_SIGNED(-0x10);
      break;
    case Kind::kExtern:
      WRITE_SIGNED(-0x11);
      break;
    case Kind::kTypeidx:
      WRITE_SIGNED(def_type_->index());
      break;
    case Kind::kAny:
      // https://github.com/v8/v8/blob/master/src/wasm/wasm-constants.h#L36
      FATAL("any/anyref not implemented by V8");
      WRITE_SIGNED(-0x12);
      break;
    case Kind::kEq:
      WRITE_SIGNED(-0x13);
      break;
    case Kind::kI31:
      // Note: https://github.com/WebAssembly/gc/blob/master/proposals/gc/MVP.md
      // lists this as -0x17, but V8 uses -0x16 in their spec and code:
      // - Spec: https://bit.ly/3cWcm6Q
      WRITE_SIGNED(-0x16);
      break;
    default:
      UNREACHABLE();
  }
}

SExpression* Rtt::Serialize(Zone* zone) {
  const auto sexp = new (zone) SExpList(zone);
  sexp->Add(new SExpSymbol("rtt"));
  sexp->Add(new SExpInteger(depth_));
  sexp->Add(heap_type_->Serialize(zone));
  return sexp;
}

void Rtt::OutputBinary(WriteStream* stream) {
  WRITE_SIGNED(-0x17);
  WRITE_UNSIGNED(depth_);
  heap_type_->OutputBinary(stream);
}

SExpression* Field::Serialize(Zone* zone) {
  return field_type_->Serialize(zone);
}

void Field::OutputBinary(WriteStream* stream) {
  field_type_->OutputBinary(stream);
}

SExpression* FieldType::Serialize(Zone* zone) {
  SExpression* sexp = nullptr;
  switch (packed_type_) {
    case PackedType::kNoType:
      sexp = value_type_->Serialize(zone);
      break;
    case PackedType::kI8:
      sexp = new (zone) SExpSymbol("i8");
      break;
    case PackedType::kI16:
      sexp = new (zone) SExpSymbol("i16");
      break;
    default:
      UNREACHABLE();
  }
  // Add mutable atom if present.
  if (mut_) {
    const auto sexp_list = new (zone) SExpList(zone);
    sexp_list->Add(new (zone) SExpSymbol("mut"));
    sexp_list->Add(sexp);
    return sexp_list;
  } else {
    return sexp;
  }
}

void FieldType::OutputBinary(WriteStream* stream) {
  switch (packed_type_) {
    case PackedType::kNoType:
      value_type_->OutputBinary(stream);
      break;
    case PackedType::kI8:
      WRITE_BYTE(0x7A);
      break;
    case PackedType::kI16:
      WRITE_BYTE(0x79);
      break;
    default:
      UNREACHABLE();
  }
  // Add mutable atom if present.
  if (mut_) {
    WRITE_BYTE(0x01);
  } else {
    WRITE_BYTE(0x00);
  }
}

FuncType::FuncType(Zone* zone, int index, ValueType* result_type)
    : DefType(zone, index), param_types_(zone, 16), result_type_(result_type) {}

SExpression* FuncType::Serialize(Zone* zone) {
  const auto sexp = new (zone) SExpList(zone);
  sexp->Add(new (zone) SExpSymbol("func"));
  // Add "param" atoms.
  for (ValueType* param_type : param_types_) {
    const auto atom = new (zone) SExpList(zone);
    atom->Add(new (zone) SExpSymbol("param"));
    atom->Add(param_type->Serialize(zone));
    sexp->Add(atom);
  }
  // Add "result" atom.
  const auto atom = new (zone) SExpList(zone);
  atom->Add(new (zone) SExpSymbol("result"));
  atom->Add(result_type_->Serialize(zone));
  sexp->Add(atom);
  return sexp;
}

void FuncType::OutputBinary(WriteStream* stream) {
  WRITE_BYTE(0x60);
  WRITE_UNSIGNED(param_types_.length());
  for (ValueType* type : param_types_) {
    type->OutputBinary(stream);
  }
  // We do not use multiple function return values in our implementation.
  WRITE_BYTE(1);
  result_type_->OutputBinary(stream);
}

void FuncType::AddParam(ValueType* param_type) {
  param_types_.Add(param_type);
}

StructType::StructType(Zone* zone, int index)
    : DefType(zone, index), fields_(zone, 16) {}

SExpression* StructType::Serialize(Zone* zone) {
  const auto sexp = new (zone) SExpList(zone);
  sexp->Add(new (zone) SExpSymbol("struct"));
  for (Field* field : fields_) {
    sexp->Add(field->Serialize(zone));
  }
  return sexp;
}

void StructType::OutputBinary(WriteStream* stream) {
  WRITE_BYTE(0x5F);
  WRITE_UNSIGNED(fields_.length());
  for (Field* field : fields_) {
    field->OutputBinary(stream);
  }
}

SExpression* ArrayType::Serialize(Zone* zone) {
  const auto sexp = new (zone) SExpList(zone);
  sexp->Add(new (zone) SExpSymbol("array"));
  sexp->Add(field_type_->Serialize(zone));
  return sexp;
}

void ArrayType::OutputBinary(WriteStream* stream) {
  WRITE_BYTE(0x5E);
  field_type_->OutputBinary(stream);
}

Field* StructType::AddField(FieldType* field_type) {
  fields_.Add(new (zone_) Field(this, field_type, fields_.length()));
  return fields_.Last();
}

Field* StructType::AddField(ValueType* value_type, bool mut) {
  return AddField(new (zone_) FieldType(value_type, mut));
}
Field* StructType::AddField(FieldType::PackedType packed_type, bool mut) {
  return AddField(new (zone_) FieldType(packed_type, mut));
}

SExpression* LocalGet::Serialize(Zone* zone) {
  return new (zone)
      SExpSymbol(OS::SCreate(zone, "local.get $%s", local_->name()));
}

void LocalGet::OutputBinary(WriteStream* stream) {
  // TODO(andreicostin): Write the logic.
  UNIMPLEMENTED();
}

SExpression* LocalSet::Serialize(Zone* zone) {
  return new (zone)
      SExpSymbol(OS::SCreate(zone, "local.set $%s", local_->name()));
}

void LocalSet::OutputBinary(WriteStream* stream) {
  // TODO(andreicostin): Write the logic.
  UNIMPLEMENTED();
}

SExpression* Int32Add::Serialize(Zone* zone) {
  return new (zone) SExpSymbol("i32.add");
}

void Int32Add::OutputBinary(WriteStream* stream) {
  WRITE_BYTE(0x6A);
}

SExpression* Constant::Serialize(Zone* zone) {
  return new (zone) SExpSymbol(OS::SCreate(zone, "i32.const %" Pu32, value_));
}

void Constant::OutputBinary(WriteStream* stream) {
  // i32.const
  WRITE_BYTE(0x41);
  WRITE_UNSIGNED(value_);
}

If::If(Zone* zone)
    : then_(new (zone) InstructionList(zone)),
      otherwise_(new (zone) InstructionList(zone)) {}

// TODO(andreicostin): Add and serialize result type.
SExpression* If::Serialize(Zone* zone) {
  // Serialize then branch.
  const auto sexp_then = new (zone) SExpList(zone);
  sexp_then->Add(new SExpSymbol("then"));
  sexp_then->Add(then_->Serialize(zone));

  // Serialize otherwise branch.
  const auto sexp_otherwise = new (zone) SExpList(zone);
  sexp_then->Add(new SExpSymbol("else"));
  sexp_then->Add(otherwise_->Serialize(zone));

  // Produce final SExpression.
  const auto sexp = new (zone) SExpList(zone);
  sexp->Add(new SExpSymbol("if"));
  sexp->Add(sexp_then);
  sexp->Add(sexp_otherwise);
  return sexp;
}

void If::OutputBinary(WriteStream* stream) {
  // TODO(andreicostin): Write the logic.
  UNIMPLEMENTED();
}

SExpression* Local::Serialize(Zone* zone) {
  const auto sexp = new (zone) SExpList(zone);
  switch (kind_) {
    case Kind::kLocal:
      sexp->Add(new (zone) SExpSymbol("local"));
      break;
    case Kind::kParam:
      sexp->Add(new (zone) SExpSymbol("param"));
      break;
    default:
      UNREACHABLE();
  }
  if (strcmp(name_, "") != 0) {
    sexp->Add(new (zone) SExpSymbol(OS::SCreate(zone, "$%s", name_)));
  }
  sexp->Add(type_->Serialize(zone));
  return sexp;
}

void Local::OutputBinary(WriteStream* stream) {
  // TODO(andreicostin): Write the logic.
  UNIMPLEMENTED();
}

InstructionList::InstructionList(Zone* zone)
    : zone_(zone), instructions_(zone, 16) {}

SExpression* InstructionList::Serialize(Zone* zone) {
  const auto sexp = new (zone) SExpList(zone);
  for (Instruction* instr : instructions_) {
    sexp->Add(instr->Serialize(zone));
  }
  return sexp;
}

void InstructionList::OutputBinary(WriteStream* stream) {
  for (Instruction* instr : instructions_) {
    instr->OutputBinary(stream);
  }
}

Instruction* InstructionList::AddLocalGet(Local* local) {
  instructions_.Add(new (zone_) LocalGet(local));
  return instructions_.Last();
}

Instruction* InstructionList::AddLocalSet(Local* local) {
  instructions_.Add(new (zone_) LocalSet(local));
  return instructions_.Last();
}

Instruction* InstructionList::AddInt32Add() {
  instructions_.Add(new (zone_) Int32Add());
  return instructions_.Last();
}

Instruction* InstructionList::AddConstant(uint32_t value) {
  instructions_.Add(new (zone_) Constant(value));
  return instructions_.Last();
}

Instruction* InstructionList::AddIf() {
  instructions_.Add(new (zone_) If(zone_));
  return instructions_.Last();
}

Function::Function(Zone* zone, const char* name, uint32_t index, FuncType* type)
    : zone_(zone),
      name_(name),
      index_(index),
      type_(type),
      locals_(zone, 16),
      body_(nullptr) {}

SExpression* Function::Serialize(Zone* zone) {
  const auto sexp = new (zone) SExpList(zone);
  sexp->Add(new (zone) SExpSymbol("func"));
  if (strcmp(name_, "") != 0) {
    sexp->Add(new (zone) SExpSymbol(OS::SCreate(zone, "$%s", name_)));
  }
  // Serialize type.
  const auto sexp_type = new (zone) SExpList(zone);
  sexp_type->Add(new (zone) SExpSymbol("type"));
  sexp_type->Add(new (zone) SExpInteger(type_->index()));
  sexp->Add(sexp_type);
  // Serialize locals.
  for (Local* it : locals_) {
    if (it->kind() == Local::Kind::kLocal) {
      sexp->Add(it->Serialize(zone));
    }
  }
  // Serialize the body.
  if (body_ != nullptr) {
    sexp->Add(body_->Serialize(zone));
  } else {
    sexp->Add(new (zone) SExpSymbol("<missing body>"));
  }
  return sexp;
}

void Function::OutputBinary(WriteStream* stream) {
  // First, output the locals.
  WRITE_UNSIGNED(locals_.length());
  for (Local* local : locals_) {
    WRITE_BYTE(1);  // One local description follows. Wasm permits
                    // compressing multiple consecutive identical locals
                    // into one. We choose not to use this feature.
    local->type_->OutputBinary(stream);
  }
  // Then, output the function body.
  if (body_ != nullptr) {
    body_->OutputBinary(stream);
  } else {
    WasmTrace("WASM MISSING FUNCTION BODY OMITTED!\n");
  }
  WRITE_BYTE(0x0B);
}

Local* Function::AddLocal(Local::Kind kind, ValueType* type, const char* name) {
  // No further params can be declared after the first
  // local in a Wasm function header.
  ASSERT(kind == Local::Kind::kLocal || locals_.is_empty() ||
         locals_.Last()->kind() == Local::Kind::kParam);
  locals_.Add(new (zone_) Local(this, kind, type, name, locals_.length()));
  return locals_.Last();
}

Local* Function::GetLocalByName(const char* name) {
  for (Local* it : locals_) {
    if (strcmp(it->name(), name) == 0) {
      return it;
    }
  }
  FATAL("Local variable by name lookup returned no matching result.");
}

Local* Function::GetLocalByIndex(int id) {
  ASSERT(0 <= id && id < locals_.length());
  return locals_[id];
}

InstructionList* Function::MakeNewBody() {
  body_ = new (zone_) InstructionList(zone_);
  return body_;
}

WasmModuleBuilder::WasmModuleBuilder(Zone* zone)
    : i32_(NumType::Kind::kI32),
      i64_(NumType::Kind::kI64),
      f32_(NumType::Kind::kF32),
      f64_(NumType::Kind::kF64),
      func_(HeapType::Kind::kFunc),
      ext_(HeapType::Kind::kExtern),
      any_(HeapType::Kind::kAny),
      eq_(HeapType::Kind::kEq),
      i31_(HeapType::Kind::kI31),
      funcref_(/*nullable =*/true, &func_),
      externref_(/*nullable =*/true, &ext_),
      anyref_(/*nullable =*/true, &any_),
      eqref_(/*nullable =*/true, &eq_),
      i31ref_(/*nullable =*/false, &i31_),
      zone_(zone),
      types_(zone, 16),
      functions_(zone, 16) {}

SExpression* WasmModuleBuilder::Serialize(Zone* zone) {
  const auto sexp = new (zone) SExpList(zone);
  sexp->Add(new (zone) SExpSymbol("module"));
  // Types section.
  for (DefType* def_type : types_) {
    const auto sexp_type = new (zone) SExpList(zone);
    sexp_type->Add(new (zone) SExpSymbol("type"));
    sexp_type->Add(def_type->Serialize(zone));
    sexp->Add(sexp_type);
  }
  // Functions section.
  // Note that in Wasm Binary Format function bodies would
  // be stored separately, in the code section.
  for (Function* fct : functions_) {
    sexp->Add(fct->Serialize(zone));
  }
  return sexp;
}

void WasmModuleBuilder::OutputTypeSection(WriteStream* stream) {
  // Type section has index 1.
  WRITE_BYTE(1);
  WRITE_BYTECOUNT();
  WRITE_UNSIGNED(types_.length());
  for (DefType* def_type : types_) {
    def_type->OutputBinary(stream);
  }
}

void WasmModuleBuilder::OutputFunctionSection(WriteStream* stream) {
  // Function section has index 3.
  WRITE_BYTE(3);
  WRITE_BYTECOUNT();
  WRITE_UNSIGNED(functions_.length());
  for (Function* function : functions_) {
    WRITE_UNSIGNED(function->type()->index());
  }
}

void WasmModuleBuilder::OutputCodeSection(WriteStream* stream) {
  // Code section has index 10.
  WRITE_BYTE(10);
  WRITE_BYTECOUNT();
  WRITE_UNSIGNED(functions_.length());
  for (Function* function : functions_) {
    // The code of each function is preceded by its bytecount to allow
    // the embedder to lazily compile functions.
    WRITE_BYTECOUNT();
    function->OutputBinary(stream);
  }
}

void WasmModuleBuilder::OutputBinary(WriteStream* stream) {
  ASSERT(stream != nullptr);
  // Magic.
  WRITE_BYTE('\0');
  WRITE_BYTE('a');
  WRITE_BYTE('s');
  WRITE_BYTE('m');
  // Version.
  WRITE_BYTE(0x01);
  WRITE_BYTE(0x00);
  WRITE_BYTE(0x00);
  WRITE_BYTE(0x00);
  // Sections come in ascending order of their indices.
  OutputTypeSection(stream);
  OutputFunctionSection(stream);
  OutputCodeSection(stream);
}

FieldType* WasmModuleBuilder::MakeFieldType(ValueType* value_type, bool mut) {
  return new (zone_) FieldType(value_type, mut);
}

FieldType* WasmModuleBuilder::MakeFieldType(FieldType::PackedType packed_type,
                                            bool mut) {
  return new (zone_) FieldType(packed_type, mut);
}

ArrayType* WasmModuleBuilder::MakeArrayType(FieldType* field_type) {
  const auto array_type =
      new (zone_) ArrayType(zone_, types_.length(), field_type);
  types_.Add(array_type);
  return array_type;
}

ArrayType* WasmModuleBuilder::MakeArrayType(ValueType* value_type, bool mut) {
  return MakeArrayType(MakeFieldType(value_type, mut));
}
ArrayType* WasmModuleBuilder::MakeArrayType(FieldType::PackedType packed_type,
                                            bool mut) {
  return MakeArrayType(MakeFieldType(packed_type, mut));
}

HeapType* WasmModuleBuilder::MakeHeapType(DefType* def_type) {
  return new (zone_) HeapType(def_type);
}

RefType* WasmModuleBuilder::MakeRefType(bool nullable, HeapType* heap_type) {
  return new (zone_) RefType(nullable, heap_type);
}

FuncType* WasmModuleBuilder::MakeFuncType(ValueType* result_type) {
  const auto fct_type =
      new (zone_) FuncType(zone_, types_.length(), result_type);
  types_.Add(fct_type);
  return fct_type;
}

StructType* WasmModuleBuilder::MakeStructType() {
  const auto str_type = new (zone_) StructType(zone_, types_.length());
  types_.Add(str_type);
  return str_type;
}

Function* WasmModuleBuilder::AddFunction(const char* name, FuncType* type) {
  functions_.Add(new (zone_) Function(zone_, name, functions_.length(), type));
  return functions_.Last();
}

}  // namespace wasm
