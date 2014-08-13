/* Copyright (C) 2014 Carl Leonardsson
 *
 * This file is part of Nidhugg.
 *
 * Nidhugg is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nidhugg is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "DPORDriver.h"

#include "CheckModule.h"
#include "Debug.h"
#include "Interpreter.h"
#include "SigSegvHandler.h"
#include "TSOInterpreter.h"
#include "TSOTraceBuilder.h"

#include <fstream>
#include <stdexcept>

#ifdef LLVM_INCLUDE_IR
#include <llvm/IR/LLVMContext.h>
#else
#include <llvm/IR/LLVMContext.h>
#endif
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>

DPORDriver::DPORDriver(const Configuration &C) :
  conf(C), mod(0) {
  std::string ErrorMsg;
  llvm::sys::DynamicLibrary::LoadLibraryPermanently(0,&ErrorMsg);
};

DPORDriver *DPORDriver::parseIRFile(const std::string &filename,
                                    const Configuration &C){
  DPORDriver *driver =
    new DPORDriver(C);
  read_file(filename,driver->src);
  driver->reparse();
  CheckModule::check_functions(driver->mod);
  return driver;
};

DPORDriver *DPORDriver::parseIR(const std::string &llvm_asm,
                                const Configuration &C){
  DPORDriver *driver =
    new DPORDriver(C);
  driver->src = llvm_asm;
  driver->reparse();
  CheckModule::check_functions(driver->mod);
  return driver;
};

DPORDriver::~DPORDriver(){
  delete mod;
};

void DPORDriver::read_file(const std::string &filename, std::string &tgt){
  std::ifstream is(filename);
  if(!is){
    throw std::logic_error("Failed to read assembly file.");
  }
  is.seekg(0,std::ios::end);
  tgt.resize(is.tellg());
  is.seekg(0,std::ios::beg);
  is.read(&tgt[0],tgt.size());
  is.close();
};

void DPORDriver::reparse(){
  delete mod;
  llvm::SMDiagnostic err;
  llvm::MemoryBuffer *buf =
    llvm::MemoryBuffer::getMemBuffer(src,"",false);
  mod = llvm::ParseIR(buf,err,llvm::getGlobalContext());
  if(!mod){
    err.print("",llvm::errs());
    throw std::logic_error("Failed to parse assembly.");
  }
  if(mod->getDataLayout().empty()){
    if(llvm::sys::IsLittleEndianHost){
      mod->setDataLayout("e");
    }else{
      mod->setDataLayout("E");
    }
  }
};

Trace DPORDriver::run_once(TraceBuilder &TB) const{
  std::string ErrorMsg;
  llvm::ExecutionEngine *EE = 0;
  switch(conf.memory_model){
  case Configuration::SC:
    EE = llvm::Interpreter::create(mod,TB,conf,&ErrorMsg);
    break;
  case Configuration::TSO:
    EE = TSOInterpreter::create(mod,static_cast<TSOTraceBuilder&>(TB),conf,&ErrorMsg);
    break;
  default:
    throw std::logic_error("DPORDriver: Unsupported memory model.");
  }

  if (!EE) {
    if (!ErrorMsg.empty()){
      throw std::logic_error("Error creating EE: " + ErrorMsg);
    }else{
      throw std::logic_error("Unknown error creating EE!");
    }
  }

  llvm::Function *EntryFn = mod->getFunction("main");
  if(!EntryFn){
    throw std::logic_error("No main function found in module.");
  }

  // Reset errno to zero on entry to main.
  errno = 0;

  // Run static constructors.
  EE->runStaticConstructorsDestructors(false);

  // Trigger compilation separately so code regions that need to be
  // invalidated will be known.
  (void)EE->getPointerToFunction(EntryFn);

  // Empty environment
  char *act_envp = 0;
  char **envp = &act_envp;

  // Run main.
  EE->runFunctionAsMain(EntryFn, {"prog"}, envp);

  // Run static destructors.
  EE->runStaticConstructorsDestructors(true);

  if(conf.check_robustness){
    static_cast<llvm::Interpreter*>(EE)->checkForCycles();
  }

  Trace t({},{},{});
  if(TB.has_error() || conf.debug_collect_all_traces){
    t = static_cast<llvm::Interpreter*>(EE)->getTrace();
  }// else avoid computing trace

  delete EE;
  return t;
};

DPORDriver::Result DPORDriver::run(){
  Result res;

  TraceBuilder *TB = 0;

  switch(conf.memory_model){
  case Configuration::SC:
    TB = new TSOTraceBuilder(conf);
    break;
  case Configuration::TSO:
    TB = new TSOTraceBuilder(conf);
    break;
  default:
    throw std::logic_error("DPORDriver: Unsupported memory model.");
  }

  SigSegvHandler::setup_signal_handler();

  int computation_count = 0;
  do{
    if((computation_count+1) % 1000 == 0){
      reparse();
    }
    Trace t = run_once(*TB);
    if(conf.debug_collect_all_traces){
      res.all_traces.push_back(t);
    }
    if(TB->sleepset_is_empty()){
      ++res.trace_count;
    }else{
      ++res.sleepset_blocked_trace_count;
    }
    ++computation_count;
    if(t.has_errors() && !res.has_errors()){
      res.error_trace = t;
    }
    if(t.has_errors() && !conf.explore_all_traces) break;
  }while(TB->reset());

  SigSegvHandler::reset_signal_handler();

  delete TB;

  return res;
};
