//===-ThinLTOCodeGenerator.cpp - LLVM Link Time Optimizer -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the Thin Link Time Optimization library. This library is
// intended to be used by linker to optimize code at link time.
//
//===----------------------------------------------------------------------===//

#include "llvm/LTO/legacy/ThinLTOCodeGenerator.h"

#ifdef HAVE_LLVM_REVISION
#include "LLVMLTORevision.h"
#endif

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/ModuleSummaryAnalysis.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/ExecutionEngine/ObjectMemoryBuffer.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/SubtargetFeature.h"
#include "llvm/Object/IRObjectFile.h"
#include "llvm/Object/ModuleSummaryIndexObjectFile.h"
#include "llvm/Support/CachePruning.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA1.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/FunctionImport.h"
#include "llvm/Transforms/IPO/Internalize.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/ObjCARC.h"
#include "llvm/Transforms/Utils/FunctionImportUtils.h"

#include <numeric>

using namespace llvm;

#define DEBUG_TYPE "thinlto"

namespace llvm {
// Flags -discard-value-names, defined in LTOCodeGenerator.cpp
extern cl::opt<bool> LTODiscardValueNames;
}

namespace {

static cl::opt<int> ThreadCount("threads",
                                cl::init(1));

static void diagnosticHandler(const DiagnosticInfo &DI) {
  DiagnosticPrinterRawOStream DP(errs());
  DI.print(DP);
  errs() << '\n';
}

// Simple helper to save temporary files for debug.
static void saveTempBitcode(const Module &TheModule, StringRef TempDir,
                            unsigned count, StringRef Suffix) {
  if (TempDir.empty())
    return;
  // User asked to save temps, let dump the bitcode file after import.
  auto SaveTempPath = TempDir + llvm::utostr(count) + Suffix;
  std::error_code EC;
  raw_fd_ostream OS(SaveTempPath.str(), EC, sys::fs::F_None);
  if (EC)
    report_fatal_error(Twine("Failed to open ") + SaveTempPath +
                       " to save optimized bitcode\n");
  WriteBitcodeToFile(&TheModule, OS, /* ShouldPreserveUseListOrder */ true);
}

static const GlobalValueSummary *
getFirstDefinitionForLinker(const GlobalValueSummaryList &GVSummaryList) {
  // If there is any strong definition anywhere, get it.
  auto StrongDefForLinker = llvm::find_if(
      GVSummaryList, [](const std::unique_ptr<GlobalValueSummary> &Summary) {
        auto Linkage = Summary->linkage();
        return !GlobalValue::isAvailableExternallyLinkage(Linkage) &&
               !GlobalValue::isWeakForLinker(Linkage);
      });
  if (StrongDefForLinker != GVSummaryList.end())
    return StrongDefForLinker->get();
  // Get the first *linker visible* definition for this global in the summary
  // list.
  auto FirstDefForLinker = llvm::find_if(
      GVSummaryList, [](const std::unique_ptr<GlobalValueSummary> &Summary) {
        auto Linkage = Summary->linkage();
        return !GlobalValue::isAvailableExternallyLinkage(Linkage);
      });
  // Extern templates can be emitted as available_externally.
  if (FirstDefForLinker == GVSummaryList.end())
    return nullptr;
  return FirstDefForLinker->get();
}

// Populate map of GUID to the prevailing copy for any multiply defined
// symbols. Currently assume first copy is prevailing, or any strong
// definition. Can be refined with Linker information in the future.
static void computePrevailingCopies(
    const ModuleSummaryIndex &Index,
    DenseMap<GlobalValue::GUID, const GlobalValueSummary *> &PrevailingCopy) {
  auto HasMultipleCopies = [&](const GlobalValueSummaryList &GVSummaryList) {
    return GVSummaryList.size() > 1;
  };

  for (auto &I : Index) {
    if (HasMultipleCopies(I.second))
      PrevailingCopy[I.first] = getFirstDefinitionForLinker(I.second);
  }
}

static StringMap<MemoryBufferRef>
generateModuleMap(const std::vector<MemoryBufferRef> &Modules) {
  StringMap<MemoryBufferRef> ModuleMap;
  for (auto &ModuleBuffer : Modules) {
    assert(ModuleMap.find(ModuleBuffer.getBufferIdentifier()) ==
               ModuleMap.end() &&
           "Expect unique Buffer Identifier");
    ModuleMap[ModuleBuffer.getBufferIdentifier()] = ModuleBuffer;
  }
  return ModuleMap;
}

static void promoteModule(Module &TheModule, const ModuleSummaryIndex &Index) {
  if (renameModuleForThinLTO(TheModule, Index))
    report_fatal_error("renameModuleForThinLTO failed");
}

static void
crossImportIntoModule(Module &TheModule, const ModuleSummaryIndex &Index,
                      StringMap<MemoryBufferRef> &ModuleMap,
                      const FunctionImporter::ImportMapTy &ImportList) {
  ModuleLoader Loader(TheModule.getContext(), ModuleMap);
  FunctionImporter Importer(Index, Loader);
  Importer.importFunctions(TheModule, ImportList);
}

static void optimizeModule(Module &TheModule, TargetMachine &TM) {
  // Populate the PassManager
  PassManagerBuilder PMB;
  PMB.LibraryInfo = new TargetLibraryInfoImpl(TM.getTargetTriple());
  PMB.Inliner = createFunctionInliningPass();
  // FIXME: should get it from the bitcode?
  PMB.OptLevel = 3;
  PMB.LoopVectorize = true;
  PMB.SLPVectorize = true;
  PMB.VerifyInput = true;
  PMB.VerifyOutput = false;

  legacy::PassManager PM;

  // Add the TTI (required to inform the vectorizer about register size for
  // instance)
  PM.add(createTargetTransformInfoWrapperPass(TM.getTargetIRAnalysis()));

  // Add optimizations
  PMB.populateThinLTOPassManager(PM);

  PM.run(TheModule);
}

// Convert the PreservedSymbols map from "Name" based to "GUID" based.
static DenseSet<GlobalValue::GUID>
computeGUIDPreservedSymbols(const StringSet<> &PreservedSymbols,
                            const Triple &TheTriple) {
  DenseSet<GlobalValue::GUID> GUIDPreservedSymbols(PreservedSymbols.size());
  for (auto &Entry : PreservedSymbols) {
    StringRef Name = Entry.first();
    if (TheTriple.isOSBinFormatMachO() && Name.size() > 0 && Name[0] == '_')
      Name = Name.drop_front();
    GUIDPreservedSymbols.insert(GlobalValue::getGUID(Name));
  }
  return GUIDPreservedSymbols;
}

std::unique_ptr<MemoryBuffer> codegenModule(Module &TheModule,
                                            TargetMachine &TM) {
  SmallVector<char, 128> OutputBuffer;

  // CodeGen
  {
    raw_svector_ostream OS(OutputBuffer);
    legacy::PassManager PM;

    // If the bitcode files contain ARC code and were compiled with optimization,
    // the ObjCARCContractPass must be run, so do it unconditionally here.
    PM.add(createObjCARCContractPass());

    // Setup the codegen now.
    if (TM.addPassesToEmitFile(PM, OS, TargetMachine::CGFT_ObjectFile,
                               /* DisableVerify */ true))
      report_fatal_error("Failed to setup codegen");

    // Run codegen now. resulting binary is in OutputBuffer.
    PM.run(TheModule);
  }
  return make_unique<ObjectMemoryBuffer>(std::move(OutputBuffer));
}

/// Manage caching for a single Module.
class ModuleCacheEntry {
  SmallString<128> EntryPath;

public:
  // Create a cache entry. This compute a unique hash for the Module considering
  // the current list of export/import, and offer an interface to query to
  // access the content in the cache.
  ModuleCacheEntry(
      StringRef CachePath, const ModuleSummaryIndex &Index, StringRef ModuleID,
      const FunctionImporter::ImportMapTy &ImportList,
      const FunctionImporter::ExportSetTy &ExportList,
      const std::map<GlobalValue::GUID, GlobalValue::LinkageTypes> &ResolvedODR,
      const GVSummaryMapTy &DefinedFunctions,
      const DenseSet<GlobalValue::GUID> &PreservedSymbols) {
    if (CachePath.empty())
      return;

    // Compute the unique hash for this entry
    // This is based on the current compiler version, the module itself, the
    // export list, the hash for every single module in the import list, the
    // list of ResolvedODR for the module, and the list of preserved symbols.

    SHA1 Hasher;

    // Start with the compiler revision
    Hasher.update(LLVM_VERSION_STRING);
#ifdef HAVE_LLVM_REVISION
    Hasher.update(LLVM_REVISION);
#endif

    // Include the hash for the current module
    auto ModHash = Index.getModuleHash(ModuleID);
    Hasher.update(ArrayRef<uint8_t>((uint8_t *)&ModHash[0], sizeof(ModHash)));
    for (auto F : ExportList)
      // The export list can impact the internalization, be conservative here
      Hasher.update(ArrayRef<uint8_t>((uint8_t *)&F, sizeof(F)));

    // Include the hash for every module we import functions from
    for (auto &Entry : ImportList) {
      auto ModHash = Index.getModuleHash(Entry.first());
      Hasher.update(ArrayRef<uint8_t>((uint8_t *)&ModHash[0], sizeof(ModHash)));
    }

    // Include the hash for the resolved ODR.
    for (auto &Entry : ResolvedODR) {
      Hasher.update(ArrayRef<uint8_t>((const uint8_t *)&Entry.first,
                                      sizeof(GlobalValue::GUID)));
      Hasher.update(ArrayRef<uint8_t>((const uint8_t *)&Entry.second,
                                      sizeof(GlobalValue::LinkageTypes)));
    }

    // Include the hash for the preserved symbols.
    for (auto &Entry : PreservedSymbols) {
      if (DefinedFunctions.count(Entry))
        Hasher.update(
            ArrayRef<uint8_t>((const uint8_t *)&Entry, sizeof(GlobalValue::GUID)));
    }

    sys::path::append(EntryPath, CachePath, toHex(Hasher.result()));
  }

  // Access the path to this entry in the cache.
  StringRef getEntryPath() { return EntryPath; }

  // Try loading the buffer for this cache entry.
  ErrorOr<std::unique_ptr<MemoryBuffer>> tryLoadingBuffer() {
    if (EntryPath.empty())
      return std::error_code();
    return MemoryBuffer::getFile(EntryPath);
  }

  // Cache the Produced object file
  std::unique_ptr<MemoryBuffer>
  write(std::unique_ptr<MemoryBuffer> OutputBuffer) {
    if (EntryPath.empty())
      return OutputBuffer;

    // Write to a temporary to avoid race condition
    SmallString<128> TempFilename;
    int TempFD;
    std::error_code EC =
        sys::fs::createTemporaryFile("Thin", "tmp.o", TempFD, TempFilename);
    if (EC) {
      errs() << "Error: " << EC.message() << "\n";
      report_fatal_error("ThinLTO: Can't get a temporary file");
    }
    {
      raw_fd_ostream OS(TempFD, /* ShouldClose */ true);
      OS << OutputBuffer->getBuffer();
    }
    // Rename to final destination (hopefully race condition won't matter here)
    EC = sys::fs::rename(TempFilename, EntryPath);
    if (EC) {
      sys::fs::remove(TempFilename);
      raw_fd_ostream OS(EntryPath, EC, sys::fs::F_None);
      if (EC)
        report_fatal_error(Twine("Failed to open ") + EntryPath +
                           " to save cached entry\n");
      OS << OutputBuffer->getBuffer();
    }
    auto ReloadedBufferOrErr = MemoryBuffer::getFile(EntryPath);
    if (auto EC = ReloadedBufferOrErr.getError()) {
      // FIXME diagnose
      errs() << "error: can't reload cached file '" << EntryPath
             << "': " << EC.message() << "\n";
      return OutputBuffer;
    }
    return std::move(*ReloadedBufferOrErr);
  }
};

static std::unique_ptr<MemoryBuffer>
ProcessThinLTOModule(Module &TheModule, ModuleSummaryIndex &Index,
                     StringMap<MemoryBufferRef> &ModuleMap, TargetMachine &TM,
                     const FunctionImporter::ImportMapTy &ImportList,
                     const FunctionImporter::ExportSetTy &ExportList,
                     const DenseSet<GlobalValue::GUID> &GUIDPreservedSymbols,
                     const GVSummaryMapTy &DefinedGlobals,
                     const ThinLTOCodeGenerator::CachingOptions &CacheOptions,
                     bool DisableCodeGen, StringRef SaveTempsDir,
                     unsigned count) {

  // "Benchmark"-like optimization: single-source case
  bool SingleModule = (ModuleMap.size() == 1);

  if (!SingleModule) {
    promoteModule(TheModule, Index);

    // Apply summary-based LinkOnce/Weak resolution decisions.
    thinLTOResolveWeakForLinkerModule(TheModule, DefinedGlobals);

    // Save temps: after promotion.
    saveTempBitcode(TheModule, SaveTempsDir, count, ".1.promoted.bc");
  }

  // Be friendly and don't nuke totally the module when the client didn't
  // supply anything to preserve.
  if (!ExportList.empty() || !GUIDPreservedSymbols.empty()) {
    // Apply summary-based internalization decisions.
    thinLTOInternalizeModule(TheModule, DefinedGlobals);
  }

  // Save internalized bitcode
  saveTempBitcode(TheModule, SaveTempsDir, count, ".2.internalized.bc");

  if (!SingleModule) {
    crossImportIntoModule(TheModule, Index, ModuleMap, ImportList);

    // Save temps: after cross-module import.
    saveTempBitcode(TheModule, SaveTempsDir, count, ".3.imported.bc");
  }

  optimizeModule(TheModule, TM);

  saveTempBitcode(TheModule, SaveTempsDir, count, ".4.opt.bc");

  if (DisableCodeGen) {
    // Configured to stop before CodeGen, serialize the bitcode and return.
    SmallVector<char, 128> OutputBuffer;
    {
      raw_svector_ostream OS(OutputBuffer);
      ModuleSummaryIndexBuilder IndexBuilder(&TheModule);
      WriteBitcodeToFile(&TheModule, OS, true, &IndexBuilder.getIndex());
    }
    return make_unique<ObjectMemoryBuffer>(std::move(OutputBuffer));
  }

  return codegenModule(TheModule, TM);
}

/// Resolve LinkOnce/Weak symbols. Record resolutions in the \p ResolvedODR map
/// for caching, and in the \p Index for application during the ThinLTO
/// backends. This is needed for correctness for exported symbols (ensure
/// at least one copy kept) and a compile-time optimization (to drop duplicate
/// copies when possible).
static void resolveWeakForLinkerInIndex(
    ModuleSummaryIndex &Index,
    StringMap<std::map<GlobalValue::GUID, GlobalValue::LinkageTypes>>
        &ResolvedODR) {

  DenseMap<GlobalValue::GUID, const GlobalValueSummary *> PrevailingCopy;
  computePrevailingCopies(Index, PrevailingCopy);

  auto isPrevailing = [&](GlobalValue::GUID GUID, const GlobalValueSummary *S) {
    const auto &Prevailing = PrevailingCopy.find(GUID);
    // Not in map means that there was only one copy, which must be prevailing.
    if (Prevailing == PrevailingCopy.end())
      return true;
    return Prevailing->second == S;
  };

  auto recordNewLinkage = [&](StringRef ModuleIdentifier,
                              GlobalValue::GUID GUID,
                              GlobalValue::LinkageTypes NewLinkage) {
    ResolvedODR[ModuleIdentifier][GUID] = NewLinkage;
  };

  thinLTOResolveWeakForLinkerInIndex(Index, isPrevailing, recordNewLinkage);
}

// Initialize the TargetMachine builder for a given Triple
static void initTMBuilder(TargetMachineBuilder &TMBuilder,
                          const Triple &TheTriple) {
  // Set a default CPU for Darwin triples (copied from LTOCodeGenerator).
  // FIXME this looks pretty terrible...
  if (TMBuilder.MCpu.empty() && TheTriple.isOSDarwin()) {
    if (TheTriple.getArch() == llvm::Triple::x86_64)
      TMBuilder.MCpu = "core2";
    else if (TheTriple.getArch() == llvm::Triple::x86)
      TMBuilder.MCpu = "yonah";
    else if (TheTriple.getArch() == llvm::Triple::aarch64)
      TMBuilder.MCpu = "cyclone";
  }
  TMBuilder.TheTriple = std::move(TheTriple);
}

} // end anonymous namespace

void ThinLTOCodeGenerator::addModule(StringRef Identifier, StringRef Data) {
  MemoryBufferRef Buffer(Data, Identifier);
  if (Modules.empty()) {
    // First module added, so initialize the triple and some options
    LLVMContext Context;
    Triple TheTriple(getBitcodeTargetTriple(Buffer, Context));
    initTMBuilder(TMBuilder, Triple(TheTriple));
  }
#ifndef NDEBUG
  else {
    LLVMContext Context;
    assert(TMBuilder.TheTriple.str() ==
               getBitcodeTargetTriple(Buffer, Context) &&
           "ThinLTO modules with different triple not supported");
  }
#endif
  Modules.push_back(Buffer);
}

void ThinLTOCodeGenerator::preserveSymbol(StringRef Name) {
  PreservedSymbols.insert(Name);
}

void ThinLTOCodeGenerator::crossReferenceSymbol(StringRef Name) {
  // FIXME: At the moment, we don't take advantage of this extra information,
  // we're conservatively considering cross-references as preserved.
  //  CrossReferencedSymbols.insert(Name);
  PreservedSymbols.insert(Name);
}

// TargetMachine factory
std::unique_ptr<TargetMachine> TargetMachineBuilder::create() const {
  std::string ErrMsg;
  const Target *TheTarget =
      TargetRegistry::lookupTarget(TheTriple.str(), ErrMsg);
  if (!TheTarget) {
    report_fatal_error("Can't load target for this Triple: " + ErrMsg);
  }

  // Use MAttr as the default set of features.
  SubtargetFeatures Features(MAttr);
  Features.getDefaultSubtargetFeatures(TheTriple);
  std::string FeatureStr = Features.getString();
  return std::unique_ptr<TargetMachine>(TheTarget->createTargetMachine(
      TheTriple.str(), MCpu, FeatureStr, Options, RelocModel,
      CodeModel::Default, CGOptLevel));
}

/**
 * Produce the combined summary index from all the bitcode files:
 * "thin-link".
 */
std::unique_ptr<ModuleSummaryIndex> ThinLTOCodeGenerator::linkCombinedIndex() {
  std::unique_ptr<ModuleSummaryIndex> CombinedIndex;
  uint64_t NextModuleId = 0;
  for (auto &ModuleBuffer : Modules) {
    ErrorOr<std::unique_ptr<object::ModuleSummaryIndexObjectFile>> ObjOrErr =
        object::ModuleSummaryIndexObjectFile::create(ModuleBuffer,
                                                     diagnosticHandler);
    if (std::error_code EC = ObjOrErr.getError()) {
      // FIXME diagnose
      errs() << "error: can't create ModuleSummaryIndexObjectFile for buffer: "
             << EC.message() << "\n";
      return nullptr;
    }
    auto Index = (*ObjOrErr)->takeIndex();
    if (CombinedIndex) {
      CombinedIndex->mergeFrom(std::move(Index), ++NextModuleId);
    } else {
      CombinedIndex = std::move(Index);
    }
  }
  return CombinedIndex;
}

/**
 * Perform promotion and renaming of exported internal functions.
 * Index is updated to reflect linkage changes from weak resolution.
 */
void ThinLTOCodeGenerator::promote(Module &TheModule,
                                   ModuleSummaryIndex &Index) {
  auto ModuleCount = Index.modulePaths().size();
  auto ModuleIdentifier = TheModule.getModuleIdentifier();
  // Collect for each module the list of function it defines (GUID -> Summary).
  StringMap<GVSummaryMapTy> ModuleToDefinedGVSummaries;
  Index.collectDefinedGVSummariesPerModule(ModuleToDefinedGVSummaries);

  // Generate import/export list
  StringMap<FunctionImporter::ImportMapTy> ImportLists(ModuleCount);
  StringMap<FunctionImporter::ExportSetTy> ExportLists(ModuleCount);
  ComputeCrossModuleImport(Index, ModuleToDefinedGVSummaries, ImportLists,
                           ExportLists);

  // Resolve LinkOnce/Weak symbols.
  StringMap<std::map<GlobalValue::GUID, GlobalValue::LinkageTypes>> ResolvedODR;
  resolveWeakForLinkerInIndex(Index, ResolvedODR);

  thinLTOResolveWeakForLinkerModule(
      TheModule, ModuleToDefinedGVSummaries[ModuleIdentifier]);

  promoteModule(TheModule, Index);
}

/**
 * Perform cross-module importing for the module identified by ModuleIdentifier.
 */
void ThinLTOCodeGenerator::crossModuleImport(Module &TheModule,
                                             ModuleSummaryIndex &Index) {
  auto ModuleMap = generateModuleMap(Modules);
  auto ModuleCount = Index.modulePaths().size();

  // Collect for each module the list of function it defines (GUID -> Summary).
  StringMap<GVSummaryMapTy> ModuleToDefinedGVSummaries(ModuleCount);
  Index.collectDefinedGVSummariesPerModule(ModuleToDefinedGVSummaries);

  // Generate import/export list
  StringMap<FunctionImporter::ImportMapTy> ImportLists(ModuleCount);
  StringMap<FunctionImporter::ExportSetTy> ExportLists(ModuleCount);
  ComputeCrossModuleImport(Index, ModuleToDefinedGVSummaries, ImportLists,
                           ExportLists);
  auto &ImportList = ImportLists[TheModule.getModuleIdentifier()];

  crossImportIntoModule(TheModule, Index, ModuleMap, ImportList);
}

/**
 * Compute the list of summaries needed for importing into module.
 */
void ThinLTOCodeGenerator::gatherImportedSummariesForModule(
    StringRef ModulePath, ModuleSummaryIndex &Index,
    std::map<std::string, GVSummaryMapTy> &ModuleToSummariesForIndex) {
  auto ModuleCount = Index.modulePaths().size();

  // Collect for each module the list of function it defines (GUID -> Summary).
  StringMap<GVSummaryMapTy> ModuleToDefinedGVSummaries(ModuleCount);
  Index.collectDefinedGVSummariesPerModule(ModuleToDefinedGVSummaries);

  // Generate import/export list
  StringMap<FunctionImporter::ImportMapTy> ImportLists(ModuleCount);
  StringMap<FunctionImporter::ExportSetTy> ExportLists(ModuleCount);
  ComputeCrossModuleImport(Index, ModuleToDefinedGVSummaries, ImportLists,
                           ExportLists);

  llvm::gatherImportedSummariesForModule(ModulePath, ModuleToDefinedGVSummaries,
                                         ImportLists,
                                         ModuleToSummariesForIndex);
}

/**
 * Emit the list of files needed for importing into module.
 */
void ThinLTOCodeGenerator::emitImports(StringRef ModulePath,
                                       StringRef OutputName,
                                       ModuleSummaryIndex &Index) {
  auto ModuleCount = Index.modulePaths().size();

  // Collect for each module the list of function it defines (GUID -> Summary).
  StringMap<GVSummaryMapTy> ModuleToDefinedGVSummaries(ModuleCount);
  Index.collectDefinedGVSummariesPerModule(ModuleToDefinedGVSummaries);

  // Generate import/export list
  StringMap<FunctionImporter::ImportMapTy> ImportLists(ModuleCount);
  StringMap<FunctionImporter::ExportSetTy> ExportLists(ModuleCount);
  ComputeCrossModuleImport(Index, ModuleToDefinedGVSummaries, ImportLists,
                           ExportLists);

  std::error_code EC;
  if ((EC = EmitImportsFiles(ModulePath, OutputName, ImportLists)))
    report_fatal_error(Twine("Failed to open ") + OutputName +
                       " to save imports lists\n");
}

/**
 * Perform internalization. Index is updated to reflect linkage changes.
 */
void ThinLTOCodeGenerator::internalize(Module &TheModule,
                                       ModuleSummaryIndex &Index) {
  initTMBuilder(TMBuilder, Triple(TheModule.getTargetTriple()));
  auto ModuleCount = Index.modulePaths().size();
  auto ModuleIdentifier = TheModule.getModuleIdentifier();

  // Convert the preserved symbols set from string to GUID
  auto GUIDPreservedSymbols =
      computeGUIDPreservedSymbols(PreservedSymbols, TMBuilder.TheTriple);

  // Collect for each module the list of function it defines (GUID -> Summary).
  StringMap<GVSummaryMapTy> ModuleToDefinedGVSummaries(ModuleCount);
  Index.collectDefinedGVSummariesPerModule(ModuleToDefinedGVSummaries);

  // Generate import/export list
  StringMap<FunctionImporter::ImportMapTy> ImportLists(ModuleCount);
  StringMap<FunctionImporter::ExportSetTy> ExportLists(ModuleCount);
  ComputeCrossModuleImport(Index, ModuleToDefinedGVSummaries, ImportLists,
                           ExportLists);
  auto &ExportList = ExportLists[ModuleIdentifier];

  // Be friendly and don't nuke totally the module when the client didn't
  // supply anything to preserve.
  if (ExportList.empty() && GUIDPreservedSymbols.empty())
    return;

  // Internalization
  auto isExported = [&](StringRef ModuleIdentifier, GlobalValue::GUID GUID) {
    const auto &ExportList = ExportLists.find(ModuleIdentifier);
    return (ExportList != ExportLists.end() &&
            ExportList->second.count(GUID)) ||
           GUIDPreservedSymbols.count(GUID);
  };
  thinLTOInternalizeAndPromoteInIndex(Index, isExported);
  thinLTOInternalizeModule(TheModule,
                           ModuleToDefinedGVSummaries[ModuleIdentifier]);
}

/**
 * Perform post-importing ThinLTO optimizations.
 */
void ThinLTOCodeGenerator::optimize(Module &TheModule) {
  initTMBuilder(TMBuilder, Triple(TheModule.getTargetTriple()));

  // Optimize now
  optimizeModule(TheModule, *TMBuilder.create());
}

/**
 * Perform ThinLTO CodeGen.
 */
std::unique_ptr<MemoryBuffer> ThinLTOCodeGenerator::codegen(Module &TheModule) {
  initTMBuilder(TMBuilder, Triple(TheModule.getTargetTriple()));
  return codegenModule(TheModule, *TMBuilder.create());
}

// Main entry point for the ThinLTO processing
void ThinLTOCodeGenerator::run() {
#if 0
  if (CodeGenOnly) {
    // Perform only parallel codegen and return.
    ThreadPool Pool;
    assert(ProducedBinaries.empty() && "The generator should not be reused");
    ProducedBinaries.resize(Modules.size());
    int count = 0;
    for (auto &ModuleBuffer : Modules) {
      Pool.async([&](int count) {
        LLVMContext Context;
        Context.setDiscardValueNames(LTODiscardValueNames);

        // Parse module now
        auto TheModule = loadModuleFromBuffer(ModuleBuffer, Context, false);

        // CodeGen
        ProducedBinaries[count] = codegen(*TheModule);
      }, count++);
    }

    return;
  }

  // Sequential linking phase
  auto Index = linkCombinedIndex();

  // Save temps: index.
  if (!SaveTempsDir.empty()) {
    auto SaveTempPath = SaveTempsDir + "index.bc";
    std::error_code EC;
    raw_fd_ostream OS(SaveTempPath, EC, sys::fs::F_None);
    if (EC)
      report_fatal_error(Twine("Failed to open ") + SaveTempPath +
                         " to save optimized bitcode\n");
    WriteIndexToFile(*Index, OS);
  }

  // Prepare the resulting object vector
  assert(ProducedBinaries.empty() && "The generator should not be reused");
  ProducedBinaries.resize(Modules.size());

  // Prepare the module map.
  auto ModuleMap = generateModuleMap(Modules);
  auto ModuleCount = Modules.size();

  // Collect for each module the list of function it defines (GUID -> Summary).
  StringMap<GVSummaryMapTy> ModuleToDefinedGVSummaries(ModuleCount);
  Index->collectDefinedGVSummariesPerModule(ModuleToDefinedGVSummaries);

  // Collect the import/export lists for all modules from the call-graph in the
  // combined index.
  StringMap<FunctionImporter::ImportMapTy> ImportLists(ModuleCount);
  StringMap<FunctionImporter::ExportSetTy> ExportLists(ModuleCount);
  ComputeCrossModuleImport(*Index, ModuleToDefinedGVSummaries, ImportLists,
                           ExportLists);

  // Convert the preserved symbols set from string to GUID, this is needed for
  // computing the caching hash and the internalization.
  auto GUIDPreservedSymbols =
      computeGUIDPreservedSymbols(PreservedSymbols, TMBuilder.TheTriple);

  // We use a std::map here to be able to have a defined ordering when
  // producing a hash for the cache entry.
  // FIXME: we should be able to compute the caching hash for the entry based
  // on the index, and nuke this map.
  StringMap<std::map<GlobalValue::GUID, GlobalValue::LinkageTypes>> ResolvedODR;

  // Resolve LinkOnce/Weak symbols, this has to be computed early because it
  // impacts the caching.
  resolveWeakForLinkerInIndex(*Index, ResolvedODR);

  auto isExported = [&](StringRef ModuleIdentifier, GlobalValue::GUID GUID) {
    const auto &ExportList = ExportLists.find(ModuleIdentifier);
    return (ExportList != ExportLists.end() &&
            ExportList->second.count(GUID)) ||
           GUIDPreservedSymbols.count(GUID);
  };

  // Use global summary-based analysis to identify symbols that can be
  // internalized (because they aren't exported or preserved as per callback).
  // Changes are made in the index, consumed in the ThinLTO backends.
  thinLTOInternalizeAndPromoteInIndex(*Index, isExported);

  // Make sure that every module has an entry in the ExportLists and
  // ResolvedODR maps to enable threaded access to these maps below.
  for (auto &DefinedGVSummaries : ModuleToDefinedGVSummaries) {
    ExportLists[DefinedGVSummaries.first()];
    ResolvedODR[DefinedGVSummaries.first()];
  }

  // Compute the ordering we will process the inputs: the rough heuristic here
  // is to sort them per size so that the largest module get schedule as soon as
  // possible. This is purely a compile-time optimization.
  std::vector<int> ModulesOrdering;
  ModulesOrdering.resize(Modules.size());
  std::iota(ModulesOrdering.begin(), ModulesOrdering.end(), 0);
  std::sort(ModulesOrdering.begin(), ModulesOrdering.end(),
            [&](int LeftIndex, int RightIndex) {
              auto LSize = Modules[LeftIndex].getBufferSize();
              auto RSize = Modules[RightIndex].getBufferSize();
              return LSize > RSize;
            });

  // Parallel optimizer + codegen
  {
    ThreadPool Pool(ThreadCount);
    for (auto IndexCount : ModulesOrdering) {
      auto &ModuleBuffer = Modules[IndexCount];
      Pool.async([&](int count) {
        auto ModuleIdentifier = ModuleBuffer.getBufferIdentifier();
        auto &ExportList = ExportLists[ModuleIdentifier];

        auto &DefinedFunctions = ModuleToDefinedGVSummaries[ModuleIdentifier];

        // The module may be cached, this helps handling it.
        ModuleCacheEntry CacheEntry(CacheOptions.Path, *Index, ModuleIdentifier,
                                    ImportLists[ModuleIdentifier], ExportList,
                                    ResolvedODR[ModuleIdentifier],
                                    DefinedFunctions, GUIDPreservedSymbols);

        {
          auto ErrOrBuffer = CacheEntry.tryLoadingBuffer();
          DEBUG(dbgs() << "Cache " << (ErrOrBuffer ? "hit" : "miss") << " '"
                       << CacheEntry.getEntryPath() << "' for buffer " << count
                       << " " << ModuleIdentifier << "\n");

          if (ErrOrBuffer) {
            // Cache Hit!
            ProducedBinaries[count] = std::move(ErrOrBuffer.get());
            return;
          }
        }

        LLVMContext Context;
        Context.setDiscardValueNames(LTODiscardValueNames);
        Context.enableDebugTypeODRUniquing();

        // Parse module now
        auto TheModule = loadModuleFromBuffer(ModuleBuffer, Context, false);

        // Save temps: original file.
        saveTempBitcode(*TheModule, SaveTempsDir, count, ".0.original.bc");

        auto &ImportList = ImportLists[ModuleIdentifier];
        // Run the main process now, and generates a binary
        auto OutputBuffer = ProcessThinLTOModule(
            *TheModule, *Index, ModuleMap, *TMBuilder.create(), ImportList,
            ExportList, GUIDPreservedSymbols,
            ModuleToDefinedGVSummaries[ModuleIdentifier], CacheOptions,
            DisableCodeGen, SaveTempsDir, count);

        OutputBuffer = CacheEntry.write(std::move(OutputBuffer));
        ProducedBinaries[count] = std::move(OutputBuffer);
      }, IndexCount);
    }
  }

  CachePruning(CacheOptions.Path)
      .setPruningInterval(CacheOptions.PruningInterval)
      .setEntryExpiration(CacheOptions.Expiration)
      .setMaxSize(CacheOptions.MaxPercentageOfAvailableSpace)
      .prune();

  // If statistics were requested, print them out now.
  if (llvm::AreStatisticsEnabled())
    llvm::PrintStatistics();
#endif
}
