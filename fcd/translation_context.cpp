//
// translation_context.cpp
// Copyright (C) 2015 Félix Cloutier.
// All Rights Reserved.
//
// This file is part of fcd.
// 
// fcd is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// fcd is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with fcd.  If not, see <http://www.gnu.org/licenses/>.
//

#include "llvm_warnings.h"
#include "metadata.h"
#include "not_null.h"
#include "translation_context.h"
#include "x86_register_map.h"

SILENCE_LLVM_WARNINGS_BEGIN()
#include <llvm/ADT/Triple.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils/Cloning.h>
SILENCE_LLVM_WARNINGS_END()

#include <array>
#include <unordered_set>
#include <vector>

using namespace llvm;
using namespace std;

extern "C" const char fcd_emulator_start_x86;
extern "C" const char fcd_emulator_end_x86;

class CodeGenerator
{
	typedef Constant* (CodeGenerator::*ConstantFromCapstone)(const cs_detail&);
	
	LLVMContext& ctx;
	unique_ptr<Module> module;
	vector<Function*> functionByOpcode;
	Function* prologue;
	
	NOT_NULL(Type) registerType;
	NOT_NULL(Type) flagsType;
	NOT_NULL(Type) configType;
	SmallVector<Value*, 3> ipOffset;
	
	ConstantFromCapstone constantBuilder;
	
	CodeGenerator(LLVMContext& ctx)
	: ctx(ctx), registerType(Type::getVoidTy(ctx)), flagsType(Type::getVoidTy(ctx)), configType(Type::getVoidTy(ctx))
	{
	}
	
	bool init(const char* begin, const char* end)
	{
		SMDiagnostic errors;
		MemoryBufferRef buffer(StringRef(begin, end - begin), "IRImplementation");
		if (auto module = parseIR(buffer, errors, ctx))
		{
			module = move(module);
			return true;
		}
		else
		{
			errors.print(nullptr, errs());
			assert(false);
			return false;
		}
	}
	
	Function* getFunction(const char* name)
	{
		auto result = module->getFunction(name);
		assert(result != nullptr);
		return result;
	}
	
	Constant* constantForX86(const cs_detail& detail)
	{
		Type* int8Ty = Type::getInt8Ty(ctx);
		Type* int32Ty = Type::getInt32Ty(ctx);
		Type* int64Ty = Type::getInt64Ty(ctx);
		
		const cs_x86& cs = detail.x86;
		StructType* x86Ty = module->getTypeByName("struct.cs_x86");
		StructType* x86Op = module->getTypeByName("struct.cs_x86_op");
		StructType* x86OpMem = module->getTypeByName("struct.x86_op_mem");
		StructType* x86OpMemWrapper = module->getTypeByName("union.anon");
		
		vector<Constant*> operands;
		for (size_t i = 0; i < 8; i++)
		{
			vector<Constant*> structFields {
				ConstantInt::get(int32Ty, cs.operands[i].mem.segment),
				ConstantInt::get(int32Ty, cs.operands[i].mem.base),
				ConstantInt::get(int32Ty, cs.operands[i].mem.index),
				ConstantInt::get(int32Ty, cs.operands[i].mem.scale),
				ConstantInt::get(int64Ty, cs.operands[i].mem.disp),
			};
			Constant* opMem = ConstantStruct::get(x86OpMem, structFields);
			Constant* wrapper = ConstantStruct::get(x86OpMemWrapper, opMem, nullptr);
			
			structFields = {
				ConstantInt::get(int32Ty, cs.operands[i].type),
				wrapper,
				ConstantInt::get(int8Ty, cs.operands[i].size),
				ConstantInt::get(int32Ty, cs.operands[i].avx_bcast),
				ConstantInt::get(int8Ty, cs.operands[i].avx_zero_opmask),
			};
			operands.push_back(ConstantStruct::get(x86Op, structFields));
		}
		
		vector<Constant*> fields = {
			ConstantDataArray::get(ctx, ArrayRef<uint8_t>(begin(cs.prefix), end(cs.prefix))),
			ConstantDataArray::get(ctx, ArrayRef<uint8_t>(begin(cs.opcode), end(cs.opcode))),
			ConstantInt::get(int8Ty, cs.rex),
			ConstantInt::get(int8Ty, cs.addr_size),
			ConstantInt::get(int8Ty, cs.modrm),
			ConstantInt::get(int8Ty, cs.sib),
			ConstantInt::get(int32Ty, cs.disp),
			ConstantInt::get(int32Ty, cs.sib_index),
			ConstantInt::get(int8Ty, cs.sib_scale),
			ConstantInt::get(int32Ty, cs.sib_base),
			ConstantInt::get(int32Ty, cs.sse_cc),
			ConstantInt::get(int32Ty, cs.avx_cc),
			ConstantInt::get(int8Ty, cs.avx_sae),
			ConstantInt::get(int32Ty, cs.avx_rm),
			ConstantInt::get(int8Ty, cs.op_count),
			ConstantArray::get(ArrayType::get(x86Op, 8), operands),
		};
		return ConstantStruct::get(x86Ty, fields);
	}
	
public:
	static unique_ptr<CodeGenerator> x86(LLVMContext& ctx)
	{
		unique_ptr<CodeGenerator> codegen(new CodeGenerator(ctx));
		if (!codegen->init(&fcd_emulator_start_x86, &fcd_emulator_end_x86))
		{
			return nullptr;
		}
		
		codegen->registerType = codegen->module->getTypeByName("struct.x86_regs");
		codegen->flagsType = codegen->module->getTypeByName("struct.x86_flags_reg");
		codegen->configType = codegen->module->getTypeByName("struct.x86_config");
		codegen->constantBuilder = &CodeGenerator::constantForX86;
		
		Type* i32 = Type::getInt32Ty(ctx);
		Type* i64 = Type::getInt64Ty(ctx);
		codegen->ipOffset = { ConstantInt::get(i64, 0), ConstantInt::get(i32, 9), ConstantInt::get(i32, 0) };
		
		codegen->prologue = codegen->getFunction("x86_function_prologue");
		auto& funcs = codegen->functionByOpcode;
		funcs.reserve(X86_INS_ENDING);
		
#define X86_INSTRUCTION_DECL(e, n) funcs[e] = codegen->getFunction("x86_" #n);
#include "x86_defs.h"
		
		return codegen;
	}
	
	Function* implementationFor(unsigned index)
	{
		return functionByOpcode.at(index);
	}
	
	Function* implementationForPrologue()
	{
		return prologue;
	}
	
	StructType* getRegisterTy() { return cast<StructType>(registerType); }
	StructType* getFlagsTy() { return cast<StructType>(flagsType); }
	StructType* getConfigTy() { return cast<StructType>(configType); }
	
	Constant* constantForDetail(const cs_detail& detail)
	{
		return (this->*constantBuilder)(detail);
	}
	
	BasicBlock* inlineInstruction(Function* target, Function* instructionBody, ArrayRef<Value*> parameters)
	{
		ValueToValueMapTy argumentMap;
		Function* toInline = instructionBody;
		auto iter = toInline->arg_begin();
		
		for (Value* parameter : parameters)
		{
			argumentMap[iter] = parameter;
			++iter;
		}
		
		BasicBlock* jumpOut = nullptr;
		SmallVector<ReturnInst*, 1> returns;
		CloneAndPruneFunctionInto(target, toInline, argumentMap, true, returns);
		if (returns.size() > 0)
		{
			jumpOut = BasicBlock::Create(target->getContext(), "", target);
			for (ReturnInst* ret : returns)
			{
				BranchInst::Create(jumpOut, ret);
				ret->eraseFromParent();
			}
		}
		
		return jumpOut;
	}
};

class AddressToFunction
{
	Module& module;
	FunctionType& fnType;
	unordered_map<uint64_t, string> aliases;
	unordered_map<uint64_t, Function*> functions;
	
	Function* createFunction(const Twine& name)
	{
		// XXX: do we really want external linkage? this has an impact on possible optimizations
		return Function::Create(&fnType, GlobalValue::ExternalLinkage, name, &module);
	}
	
public:
	AddressToFunction(Module& module, FunctionType& fnType)
	: module(module), fnType(fnType)
	{
	}
	
	void setAlias(string alias, uint64_t address)
	{
		auto result = aliases.insert({address, move(alias)});
		assert(result.second);
	}
	
	string nameForAddress(uint64_t address)
	{
		auto iter = aliases.find(address);
		if (iter != aliases.end())
		{
			return iter->second;
		}
		
		string result;
		(raw_string_ostream(result) << "func_").write_hex(address);
		return result;
	}
	
	Function* getCallTarget(uint64_t address)
	{
		Function*& result = functions[address];
		
		if (result == nullptr)
		{
			result = createFunction(nameForAddress(address));
			// Give it a body but mark it as a "prototype".
			// This is necessary because you can't attach metadata to a function without a body;
			// however, we rely on metadata to figure out whether a function must have its arguments recovered.
			LLVMContext& ctx = module.getContext();
			Type* voidTy = Type::getVoidTy(ctx);
			Type* i8Ptr = Type::getInt8PtrTy(ctx);
			FunctionType* protoIntrinType = FunctionType::get(voidTy, { i8Ptr }, false);
			Function* protoIntrin = cast<Function>(module.getOrInsertFunction("/fcd/prototype", protoIntrinType));
			BasicBlock* body = BasicBlock::Create(ctx, "", result);
			auto bitcast = CastInst::Create(CastInst::BitCast, result->arg_begin(), i8Ptr, "", body);
			CallInst::Create(protoIntrin, {bitcast}, "", body);
			ReturnInst::Create(ctx, body);
			
			md::setPrototype(*result);
			md::setVirtualAddress(*result, address);
		}
		return result;
	}
	
	Function* createFunction(uint64_t address)
	{
		Function*& result = functions[address];
		if (result == nullptr)
		{
			result = createFunction(nameForAddress(address));
			return result;
		}
		else if (md::isPrototype(*result))
		{
			result->deleteBody();
			BasicBlock::Create(result->getContext(), "entry", result);
			md::setVirtualAddress(*result, address);
			return result;
		}
		else
		{
			return nullptr;
		}
	}
};

namespace
{
	cs_mode cs_size_mode(size_t address_size)
	{
		switch (address_size)
		{
			case 2: return CS_MODE_16;
			case 4: return CS_MODE_32;
			case 8: return CS_MODE_64;
			default: throw invalid_argument("address_size");
		}
	}
	
	class AddressToBlock
	{
		Function& insertInto;
		unordered_map<uint64_t, BasicBlock*> blocks;
		unordered_map<uint64_t, BasicBlock*> stubs;
		
	public:
		AddressToBlock(Function& fn)
		: insertInto(fn)
		{
		}
		
		BasicBlock* blockToInstruction(uint64_t address)
		{
			auto iter = blocks.find(address);
			if (iter != blocks.end())
			{
				return iter->second;
			}
			
			BasicBlock*& stub = stubs[address];
			if (stub == nullptr)
			{
				stub = BasicBlock::Create(insertInto.getContext(), "", &insertInto);
			}
			return stub;
		}
		
		BasicBlock* implementInstruction(uint64_t address)
		{
			BasicBlock*& bodyBlock = blocks[address];
			if (bodyBlock != nullptr)
			{
				return nullptr;
			}
			
			bodyBlock = BasicBlock::Create(insertInto.getContext(), "", &insertInto);
			
			auto iter = stubs.find(address);
			if (iter != stubs.end())
			{
				iter->second->replaceAllUsesWith(bodyBlock);
				iter->second->eraseFromParent();
				stubs.erase(iter);
			}
			return bodyBlock;
		}
	};
	
	Type* getStoreType(LLVMContext& ctx, size_t size)
	{
		if (size == 1 || size == 2 || size == 4 || size == 8)
		{
			return Type::getIntNTy(ctx, static_cast<unsigned>(size * 8));
		}
		throw invalid_argument("size");
	}
	
	void resolveIntrinsic(CallInst& call, AddressToFunction& funcMap, AddressToBlock& blockMap, unordered_set<uint64_t>& newLabels)
	{
		Function* called = call.getCalledFunction();
		if (called == nullptr)
		{
			return;
		}
		
		auto name = called->getName();
		if (name == "x86_jump_intrin")
		{
			if (auto constantDestination = dyn_cast<ConstantInt>(call.getOperand(2)))
			{
				uint64_t dest = constantDestination->getLimitedValue();
				BasicBlock* destination = blockMap.blockToInstruction(dest);
				BranchInst* branch = BranchInst::Create(destination, &call);
				newLabels.insert(dest);
				
				BasicBlock* remaining = branch->getParent()->splitBasicBlock(&call);
				remaining->eraseFromParent();
			}
		}
		else if (name == "x86_call_intrin")
		{
			if (auto constantDestination = dyn_cast<ConstantInt>(call.getOperand(2)))
			{
				uint64_t destination = constantDestination->getLimitedValue();
				Function* target = funcMap.getCallTarget(destination);
				CallInst* replacement = CallInst::Create(target, {call.getOperand(1)}, "", &call);
				call.replaceAllUsesWith(replacement);
				call.eraseFromParent();
			}
		}
		else if (name == "x86_ret_intrin")
		{
			BasicBlock* parent = call.getParent();
			BasicBlock* remainder = parent->splitBasicBlock(&call);
			ReturnInst::Create(parent->getContext(), parent);
			remainder->eraseFromParent();
		}
		else if (name == "x86_read_mem")
		{
			Value* intptr = call.getOperand(0);
			Type* storeType = getStoreType(call.getContext(), cast<ConstantInt>(call.getOperand(1))->getLimitedValue());
			CastInst* pointer = CastInst::Create(CastInst::IntToPtr, intptr, storeType->getPointerTo(), "", &call);
			Instruction* replacement = new LoadInst(pointer, "", &call);
			md::setProgramMemory(*replacement);
			
			Type* i64 = Type::getInt64Ty(call.getContext());
			if (replacement->getType() != i64)
			{
				replacement = CastInst::Create(Instruction::ZExt, replacement, i64, "", &call);
			}
			call.replaceAllUsesWith(replacement);
			call.eraseFromParent();
		}
		else if (name == "x86_write_mem")
		{
			Value* intptr = call.getOperand(0);
			Value* value = call.getOperand(2);
			Type* storeType = getStoreType(call.getContext(), cast<ConstantInt>(call.getOperand(1))->getLimitedValue());
			CastInst* pointer = CastInst::Create(CastInst::IntToPtr, intptr, storeType->getPointerTo(), "", &call);
			
			if (value->getType() != storeType)
			{
				// Assumption: storeType can only be smaller than the type of storeValue
				value = CastInst::Create(Instruction::Trunc, value, storeType, "", &call);
			}
			StoreInst* storeInst = new StoreInst(value, pointer, &call);
			md::setProgramMemory(*storeInst);
			call.eraseFromParent();
		}
	}
	
	void resolveIntrinsics(BasicBlock& block, AddressToFunction& funcMap, AddressToBlock& blockMap, unordered_set<uint64_t>& newLabels)
	{
		vector<CallInst*> calls;
		for (Instruction& inst : block)
		{
			if (auto call = dyn_cast<CallInst>(&inst))
			{
				calls.push_back(call);
			}
		}
		
		for (CallInst* call : calls)
		{
			resolveIntrinsic(*call, funcMap, blockMap, newLabels);
		}
	}
	
	void resolveIntrinsics(BasicBlock* begin, BasicBlock* inclusiveEnd, AddressToFunction& funcMap, AddressToBlock& blockMap, unordered_set<uint64_t>& newLabels)
	{
		for (auto iter = begin; iter != inclusiveEnd; iter = iter->getNextNode())
		{
			resolveIntrinsics(*iter, funcMap, blockMap, newLabels);
		}
		resolveIntrinsics(*inclusiveEnd, funcMap, blockMap, newLabels);
	}
	
	uint64_t takeOne(unordered_set<uint64_t>& toVisit)
	{
		auto iter = toVisit.begin();
		auto result = *iter;
		toVisit.erase(iter);
		return result;
	}
}

TranslationContext::TranslationContext(LLVMContext& context, const x86_config& config, const std::string& module_name)
: context(context)
, module(new Module(module_name, context))
{
	if (auto generator = CodeGenerator::x86(context))
	{
		irgen = move(generator);
	}
	else
	{
		// This is REALLY not supposed to happen. The parameters are static.
		// XXX: If/when we have other architectures, change this to something non-fatal.
		errs() << "couldn't create IR generation module";
		abort();
	}
	
	if (auto csHandle = capstone::create(CS_ARCH_X86, CS_MODE_LITTLE_ENDIAN | cs_size_mode(config.address_size)))
	{
		cs.reset(new capstone(move(csHandle.get())));
	}
	else
	{
		errs() << "couldn't open Capstone handle: " << csHandle.getError().message() << '\n';
		abort();
	}
	
	resultFnTy = FunctionType::get(Type::getVoidTy(context), { irgen->getRegisterTy()->getPointerTo() }, false);
	functionMap.reset(new AddressToFunction(*module, *resultFnTy));
	
	Type* int32Ty = Type::getInt32Ty(context);
	Type* int64Ty = Type::getInt64Ty(context);
	StructType* configTy = irgen->getConfigTy();
	Constant* configConstant = ConstantStruct::get(configTy,
		ConstantInt::get(int32Ty, config.isa),
		ConstantInt::get(int64Ty, config.address_size),
		ConstantInt::get(int32Ty, config.ip),
		ConstantInt::get(int32Ty, config.sp),
		ConstantInt::get(int32Ty, config.fp),
		nullptr);

	configVariable = new GlobalVariable(*module, configTy, true, GlobalVariable::PrivateLinkage, configConstant, "config");
	
	string dataLayout;
	// endianness (little)
	dataLayout += "e-";
	
	// native integer types (at least 8 and 16 bytes; very often 32; often 64)
	dataLayout += "n8:16";
	if (config.isa >= x86_isa32)
	{
		dataLayout += ":32";
	}
	if (config.isa >= x86_isa64)
	{
		dataLayout += ":64";
	}
	dataLayout += "-";
	
	// Pointer size
	// Irrelevant for address space 0, since this is the register address space and these pointers are never stored
	// to memory.
	dataLayout += "p0:64:64:64-";
	
	// address space 1 (memory address space)
	char addressSize[] = ":512";
	snprintf(addressSize, sizeof addressSize, ":%zu", config.address_size * 8);
	dataLayout += string("p1") + addressSize + addressSize + addressSize;
	module->setDataLayout(dataLayout);
	
	Triple triple;
	switch (config.isa)
	{
		case x86_isa32: triple.setArch(Triple::x86); break;
		case x86_isa64: triple.setArch(Triple::x86_64); break;
		default: llvm_unreachable("x86 ISA cannot map to target triple architecture");
	}
	triple.setOS(Triple::UnknownOS);
	triple.setVendor(Triple::UnknownVendor);
	
	module->setTargetTriple(triple.str());
}

TranslationContext::~TranslationContext()
{
}

string TranslationContext::nameOf(uint64_t address) const
{
	return functionMap->nameForAddress(address);
}

void TranslationContext::createAlias(uint64_t address, const std::string& name)
{
	functionMap->setAlias(name, address);
}

Function* TranslationContext::createFunction(uint64_t base_address, const uint8_t* begin, const uint8_t* end)
{
	Function* fn = functionMap->createFunction(base_address);
	assert(fn != nullptr);
	
	AddressToBlock blockMap(*fn);
	BasicBlock* entry = BasicBlock::Create(context, "entry", fn);
	
	Argument* registers = fn->arg_begin();
	auto flags = new AllocaInst(irgen->getFlagsTy(), "flags", entry);
	BasicBlock* prologueExit = irgen->inlineInstruction(fn, irgen->implementationForPrologue(), { configVariable, registers });
	BranchInst::Create(blockMap.blockToInstruction(base_address), prologueExit);
	
	unordered_set<uint64_t> toVisit { base_address };
	SmallVector<Value*, 4> parameters = { configVariable, nullptr, registers, flags };
	while (!toVisit.empty())
	{
		uint64_t branch = takeOne(toVisit);
		const uint8_t* code = begin + (branch - base_address);
		auto iter = cs->begin(code, end, branch);
		
		BasicBlock* start = &fn->back();
		for (auto next_result = iter.next(); next_result == capstone_iter::success; next_result = iter.next())
		{
			if (blockMap.implementInstruction(iter->address) == nullptr)
			{
				// already implemented
				break;
			}
			
			Function* implementation = irgen->implementationFor(iter->id);
			Constant* detailAsConstant = irgen->constantForDetail(*iter->detail);
			parameters[1] = new GlobalVariable(*module, detailAsConstant->getType(), true, GlobalValue::PrivateLinkage, detailAsConstant);
			
			BasicBlock* nextInst = irgen->inlineInstruction(fn, implementation, parameters);
			if (nextInst == nullptr)
			{
				// terminator instruction
				break;
			}
		}
		BasicBlock* end = &fn->back();
		resolveIntrinsics(start, end, *functionMap, blockMap, toVisit);
	}
	
#if DEBUG
	// check that it still works
	if (verifyModule(*module, &errs()))
	{
		module->dump();
		abort();
	}
#endif
	
	return fn;
}

unique_ptr<Module> TranslationContext::take()
{
	return move(module);
}
