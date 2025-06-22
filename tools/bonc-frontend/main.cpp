/* -*- mode: c++; c-basic-offset: 2; -*- */

//===-- main.cpp ------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/ADT/KTest.h"
#include "klee/ADT/TreeStream.h"
#include "klee/Config/Version.h"
#include "klee/Core/Interpreter.h"
#include "klee/Expr/Expr.h"
#include "klee/Solver/SolverCmdLine.h"
#include "klee/Statistics/Statistics.h"
#include "klee/Support/Debug.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/FileHandling.h"
#include "klee/Support/ModuleUtil.h"
#include "klee/Support/OptionCategories.h"
#include "klee/Support/PrintVersion.h"
#include "klee/System/Time.h"

#include "klee/Support/CompilerWarning.h"
DISABLE_WARNING_PUSH
DISABLE_WARNING_DEPRECATED_DECLARATIONS
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Errno.h"
#include "llvm/Support/FileSystem.h"
#if LLVM_VERSION_CODE >= LLVM_VERSION(16, 0)
#include "llvm/TargetParser/Host.h"
#else
#include "llvm/Support/Host.h"
#endif
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"
DISABLE_WARNING_POP

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

using namespace llvm;
using namespace klee;

namespace {
cl::opt<std::string> InputFile(cl::desc("<input bytecode>"), cl::Positional,
                               cl::init("-"));

cl::OptionCategory StartCat("Startup options",
                            "These options affect how execution is started.");

cl::opt<std::string> EntryPoint(
    "entry-point",
    cl::desc("Function in which to start execution (default=bonc_main)"),
    cl::init("bonc_main"), cl::cat(StartCat));

cl::opt<std::string>
    RunInDir("run-in-dir",
             cl::desc("Change to the given directory before starting execution "
                      "(default=location of tested file)."),
             cl::cat(StartCat));

cl::opt<std::string> OutputDir(
    "output-dir",
    cl::desc("Directory in which to write results (default=klee-out-<N>)"),
    cl::init(""), cl::cat(StartCat));

cl::opt<std::string> Environ(
    "env-file",
    cl::desc("Parse environment from the given file (in \"env\" format)"),
    cl::cat(StartCat));

cl::opt<bool> WarnAllExternals(
    "warn-all-external-symbols",
    cl::desc(
        "Issue a warning on startup for all external symbols (default=false)."),
    cl::cat(StartCat));

/*** Linking options ***/

cl::OptionCategory LinkCat("Linking options",
                           "These options control the libraries being linked.");

enum class LibcType { FreestandingLibc, KleeLibc, UcLibc };

cl::opt<LibcType> Libc(
    "libc", cl::desc("Choose libc version (none by default)."),
    cl::values(
        clEnumValN(
            LibcType::FreestandingLibc, "none",
            "Don't link in a libc (only provide freestanding environment)"),
        clEnumValN(LibcType::KleeLibc, "klee", "Link in KLEE's libc"),
        clEnumValN(LibcType::UcLibc, "uclibc",
                   "Link in uclibc (adapted for KLEE)")),
    cl::init(LibcType::FreestandingLibc), cl::cat(LinkCat));

cl::list<std::string>
    LinkLibraries("link-llvm-lib",
                  cl::desc("Link the given bitcode library before execution, "
                           "e.g. .bca, .bc, .a. Can be used multiple times."),
                  cl::value_desc("bitcode library file"), cl::cat(LinkCat));

cl::opt<bool> WithPOSIXRuntime(
    "posix-runtime",
    cl::desc("Link with POSIX runtime. Options that can be passed as arguments "
             "to the programs are: --sym-arg <max-len>  --sym-args <min-argvs> "
             "<max-argvs> <max-len> + file model options (default=false)."),
    cl::init(false), cl::cat(LinkCat));

cl::opt<bool> WithUBSanRuntime("ubsan-runtime",
                               cl::desc("Link with UBSan runtime."),
                               cl::init(false), cl::cat(LinkCat));

cl::opt<std::string> RuntimeBuild(
    "runtime-build",
    cl::desc("Link with versions of the runtime library that were built with "
             "the provided configuration (default=" RUNTIME_CONFIGURATION ")."),
    cl::init(RUNTIME_CONFIGURATION), cl::cat(LinkCat));

/*** Checks options ***/

cl::OptionCategory
    ChecksCat("Checks options",
              "These options control some of the checks being done by KLEE.");

cl::opt<bool>
    CheckDivZero("check-div-zero",
                 cl::desc("Inject checks for division-by-zero (default=true)"),
                 cl::init(true), cl::cat(ChecksCat));

cl::opt<bool>
    CheckOvershift("check-overshift",
                   cl::desc("Inject checks for overshift (default=true)"),
                   cl::init(true), cl::cat(ChecksCat));

cl::opt<bool>
    OptExitOnError("exit-on-error",
                   cl::desc("Exit KLEE if an error in the tested application "
                            "has been found (default=false)"),
                   cl::init(false), cl::cat(TerminationCat));

} // namespace

namespace klee {
extern cl::opt<std::string> MaxTime;
class ExecutionState;
} // namespace klee

/***/

class KleeHandler : public InterpreterHandler {
private:
  Interpreter *m_interpreter;
  std::unique_ptr<llvm::raw_ostream> m_infoFile;

  SmallString<128> m_outputDirectory;

public:
  KleeHandler();
  ~KleeHandler();

  llvm::raw_ostream &getInfoStream() const { return *m_infoFile; }

  void setInterpreter(Interpreter *i);

  std::string getOutputFilename(const std::string &filename);
  std::unique_ptr<llvm::raw_fd_ostream>
  openOutputFile(const std::string &filename);

  static std::string getRunTimeLibraryPath(const char *argv0);
};

KleeHandler::KleeHandler() : m_interpreter(0), m_outputDirectory() {

  // create output directory (OutputDir or "klee-out-<i>")
  bool dir_given = OutputDir != "";
  SmallString<128> directory(dir_given ? OutputDir : InputFile);

  if (!dir_given)
    sys::path::remove_filename(directory);
  if (auto ec = sys::fs::make_absolute(directory)) {
    klee_error("unable to determine absolute path: %s", ec.message().c_str());
  }

  if (dir_given) {
    // OutputDir
    if (mkdir(directory.c_str(), 0775) < 0)
      klee_error("cannot create \"%s\": %s", directory.c_str(),
                 strerror(errno));

    m_outputDirectory = directory;
  } else {
    // "klee-out-<i>"
    int i = 0;
    for (; i < INT_MAX; ++i) {
      SmallString<128> d(directory);
      llvm::sys::path::append(d, "klee-out-");
      raw_svector_ostream ds(d);
      ds << i;
      // SmallString is always up-to-date, no need to flush. See
      // Support/raw_ostream.h

      // create directory and try to link klee-last
      if (mkdir(d.c_str(), 0775) == 0) {
        m_outputDirectory = d;

        // SmallString<128> klee_last(directory);
        // llvm::sys::path::append(klee_last, "klee-last");

        // if ((unlink(klee_last.c_str()) < 0) && (errno != ENOENT)) {
        //   klee_warning("cannot remove existing klee-last symlink: %s",
        //                strerror(errno));
        // }

        // size_t offset = m_outputDirectory.size() -
        //                 llvm::sys::path::filename(m_outputDirectory).size();
        // if (symlink(m_outputDirectory.c_str() + offset, klee_last.c_str()) <
        //     0) {
        //   klee_warning("cannot create klee-last symlink: %s",
        //   strerror(errno));
        // }

        break;
      }

      // otherwise try again or exit on error
      if (errno != EEXIST)
        klee_error("cannot create \"%s\": %s", m_outputDirectory.c_str(),
                   strerror(errno));
    }
    if (i == INT_MAX && m_outputDirectory.str() == "")
      klee_error("cannot create output directory: index out of range");
  }

  klee_message("output directory is \"%s\"", m_outputDirectory.c_str());

  // open warnings.txt
  std::string file_path = getOutputFilename("warnings.txt");
  if ((klee_warning_file = fopen(file_path.c_str(), "w")) == NULL)
    klee_error("cannot open file \"%s\": %s", file_path.c_str(),
               strerror(errno));

  // open messages.txt
  file_path = getOutputFilename("messages.txt");
  if ((klee_message_file = fopen(file_path.c_str(), "w")) == NULL)
    klee_error("cannot open file \"%s\": %s", file_path.c_str(),
               strerror(errno));

  // open info
  m_infoFile = openOutputFile("info");
}

KleeHandler::~KleeHandler() {
  fclose(klee_warning_file);
  fclose(klee_message_file);
}

void KleeHandler::setInterpreter(Interpreter *i) { m_interpreter = i; }

std::string KleeHandler::getOutputFilename(const std::string &filename) {
  SmallString<128> path = m_outputDirectory;
  sys::path::append(path, filename);
  return path.c_str();
}

std::unique_ptr<llvm::raw_fd_ostream>
KleeHandler::openOutputFile(const std::string &filename) {
  std::string Error;
  std::string path = getOutputFilename(filename);
  auto f = klee_open_output_file(path, Error);
  if (!f) {
    klee_warning("error opening file \"%s\".  KLEE may have run out of file "
                 "descriptors: try to increase the maximum number of open file "
                 "descriptors by using ulimit (%s).",
                 path.c_str(), Error.c_str());
    return nullptr;
  }
  return f;
}

std::string KleeHandler::getRunTimeLibraryPath(const char *argv0) {
  // allow specifying the path to the runtime library
  const char *env = getenv("KLEE_RUNTIME_LIBRARY_PATH");
  if (env)
    return std::string(env);

  // Take any function from the execution binary but not main (as not allowed by
  // C++ standard)
  void *MainExecAddr = (void *)(intptr_t)getRunTimeLibraryPath;
  SmallString<128> toolRoot(
      llvm::sys::fs::getMainExecutable(argv0, MainExecAddr));

  // Strip off executable so we have a directory path
  llvm::sys::path::remove_filename(toolRoot);

  SmallString<128> libDir;

  if (strlen(KLEE_INSTALL_BIN_DIR) != 0 &&
      strlen(KLEE_INSTALL_RUNTIME_DIR) != 0 &&
#if LLVM_VERSION_CODE >= LLVM_VERSION(16, 0)
      toolRoot.str().ends_with(KLEE_INSTALL_BIN_DIR)
#else
      toolRoot.str().endswith(KLEE_INSTALL_BIN_DIR)
#endif
  ) {
    KLEE_DEBUG_WITH_TYPE("klee_runtime",
                         llvm::dbgs()
                             << "Using installed KLEE library runtime: ");
    libDir = toolRoot.str().substr(0, toolRoot.str().size() -
                                          strlen(KLEE_INSTALL_BIN_DIR));
    llvm::sys::path::append(libDir, KLEE_INSTALL_RUNTIME_DIR);
  } else {
    KLEE_DEBUG_WITH_TYPE("klee_runtime",
                         llvm::dbgs()
                             << "Using build directory KLEE library runtime :");
    libDir = KLEE_DIR;
    llvm::sys::path::append(libDir, "runtime/lib");
  }

  KLEE_DEBUG_WITH_TYPE("klee_runtime", llvm::dbgs() << libDir.c_str() << "\n");
  return libDir.c_str();
}

//===----------------------------------------------------------------------===//
// main Driver function
//

static Function *mainFn = nullptr;
static Function *entryFn = nullptr;

static void parseArguments(int argc, char **argv) {
  cl::SetVersionPrinter(klee::printVersion);
  // This version always reads response files
  cl::ParseCommandLineOptions(argc, argv, " klee\n");
}

static void
preparePOSIX(std::vector<std::unique_ptr<llvm::Module>> &loadedModules,
             llvm::StringRef libCPrefix) {
  mainFn->setName("__klee_posix_wrapped_main");

  // Add a definition of the main function if needed. This is the case if we
  // link against a libc implementation. Preparing for libc linking (i.e.
  // linking with uClibc will expect a main function and rename it to
  // _user_main. We just provide the definition here.
  if (!libCPrefix.empty() && !mainFn->getParent()->getFunction("main"))
    llvm::Function::Create(mainFn->getFunctionType(),
                           llvm::Function::ExternalLinkage, "main",
                           mainFn->getParent());

  llvm::Function *wrapper = nullptr;
  for (auto &module : loadedModules) {
    wrapper = module->getFunction("__klee_posix_wrapper");
    if (wrapper)
      break;
  }
  assert(wrapper && "klee_posix_wrapper not found");

  // Rename the POSIX wrapper to prefixed entrypoint, e.g. _user_main as uClibc
  // would expect it or main otherwise
  wrapper->setName(libCPrefix + "main");
}

// This is a terrible hack until we get some real modeling of the
// system. All we do is check the undefined symbols and warn about
// any "unrecognized" externals and about any obviously unsafe ones.

// Symbols we explicitly support
static const char *modelledExternals[] = {
    "_ZTVN10__cxxabiv117__class_type_infoE",
    "_ZTVN10__cxxabiv120__si_class_type_infoE",
    "_ZTVN10__cxxabiv121__vmi_class_type_infoE",

    // special functions
    "_assert",
    "__assert_fail",
    "__assert_rtn",
    "__errno_location",
    "__error",
    "calloc",
    "_exit",
    "_Exit",
    "exit",
    "free",
    "abort",
    "klee_abort",
    "klee_assume",
    "klee_check_memory_access",
    "klee_define_fixed_object",
    "klee_get_errno",
    "klee_get_valuef",
    "klee_get_valued",
    "klee_get_valuel",
    "klee_get_valuell",
    "klee_get_value_i32",
    "klee_get_value_i64",
    "klee_get_obj_size",
    "klee_is_symbolic",
    "klee_make_symbolic",
    "klee_mark_global",
    "klee_open_merge",
    "klee_close_merge",
    "klee_prefer_cex",
    "klee_posix_prefer_cex",
    "klee_print_expr",
    "klee_print_range",
    "klee_report_error",
    "klee_set_forking",
    "klee_silent_exit",
    "klee_warning",
    "klee_warning_once",
    "klee_stack_trace",
#ifdef SUPPORT_KLEE_EH_CXX
    "_klee_eh_Unwind_RaiseException_impl",
    "klee_eh_typeid_for",
#endif
    "llvm.dbg.declare",
    "llvm.dbg.value",
    "llvm.va_start",
    "llvm.va_end",
    "malloc",
    "realloc",
    "bonc_input",
    "bonc_input_plaintext",
    "bonc_input_message",
    "bonc_input_key",
    "bonc_input_iv",
    "bonc_input_nonce",
    "bonc_metaparam_round_number",
    "bonc_output",
    "bonc_output_ciphertext",
    "bonc_output_keystream",
    "bonc_output_tag",
    "llvm.bonc.round.enter",
    "llvm.bonc.round.exit",
    "memalign",
    "_ZdaPv",
    "_ZdlPv",
    "_Znaj",
    "_Znwj",
    "_Znam",
    "_Znwm",
};

// Symbols we aren't going to warn about
static const char *dontCareExternals[] = {
#if 0
  // stdio
  "fprintf",
  "fflush",
  "fopen",
  "fclose",
  "fputs_unlocked",
  "putchar_unlocked",
  "vfprintf",
  "fwrite",
  "puts",
  "printf",
  "stdin",
  "stdout",
  "stderr",
  "_stdio_term",
  "__errno_location",
  "fstat",
#endif

    // static information, pretty ok to return
    "getegid",
    "geteuid",
    "getgid",
    "getuid",
    "getpid",
    "gethostname",
    "getpgrp",
    "getppid",
    "getpagesize",
    "getpriority",
    "getgroups",
    "getdtablesize",
    "getrlimit",
    "getrlimit64",
    "getcwd",
    "getwd",
    "gettimeofday",
    "uname",

    // fp stuff we just don't worry about yet
    "frexp",
    "ldexp",
    "__isnan",
    "__signbit",
};

// Extra symbols we aren't going to warn about with klee-libc
static const char *dontCareKlee[] = {
    "__ctype_b_loc",
    "__ctype_get_mb_cur_max",

    // I/O system calls
    "open",
    "write",
    "read",
    "close",
};

// Extra symbols we aren't going to warn about with uclibc
static const char *dontCareUclibc[] = {
    "__dso_handle",

    // Don't warn about these since we explicitly commented them out of
    // uclibc.
    "printf", "vprintf"};

// Symbols we consider unsafe
static const char *unsafeExternals[] = {
    "fork",  // oh lord
    "exec",  // heaven help us
    "error", // calls _exit
    "raise", // yeah
    "kill",  // mmmhmmm
};

#define NELEMS(array) (sizeof(array) / sizeof(array[0]))
void externalsAndGlobalsCheck(const llvm::Module *m) {
  std::map<std::string, bool> externals;
  std::set<std::string> modelled(modelledExternals,
                                 modelledExternals + NELEMS(modelledExternals));
  std::set<std::string> dontCare(dontCareExternals,
                                 dontCareExternals + NELEMS(dontCareExternals));
  std::set<std::string> unsafe(unsafeExternals,
                               unsafeExternals + NELEMS(unsafeExternals));

  switch (Libc) {
  case LibcType::KleeLibc:
    dontCare.insert(dontCareKlee, dontCareKlee + NELEMS(dontCareKlee));
    break;
  case LibcType::UcLibc:
    dontCare.insert(dontCareUclibc, dontCareUclibc + NELEMS(dontCareUclibc));
    break;
  case LibcType::FreestandingLibc: /* silence compiler warning */
    break;
  }

  if (WithPOSIXRuntime)
    dontCare.insert("syscall");

  for (Module::const_iterator fnIt = m->begin(), fn_ie = m->end();
       fnIt != fn_ie; ++fnIt) {
    if (fnIt->isDeclaration() && !fnIt->use_empty())
      externals.insert(std::make_pair(fnIt->getName(), false));
  }

  for (Module::const_global_iterator it = m->global_begin(),
                                     ie = m->global_end();
       it != ie; ++it)
    if (it->isDeclaration() && !it->use_empty())
      externals.insert(std::make_pair(it->getName(), true));
  // and remove aliases (they define the symbol after global
  // initialization)
  for (Module::const_alias_iterator it = m->alias_begin(), ie = m->alias_end();
       it != ie; ++it) {
    std::map<std::string, bool>::iterator it2 =
        externals.find(it->getName().str());
    if (it2 != externals.end())
      externals.erase(it2);
  }

  std::map<std::string, bool> foundUnsafe;
  for (std::map<std::string, bool>::iterator it = externals.begin(),
                                             ie = externals.end();
       it != ie; ++it) {
    const std::string &ext = it->first;
    if (!modelled.count(ext) && (WarnAllExternals || !dontCare.count(ext))) {
      if (ext.compare(0, 5, "llvm.") != 0) { // not an LLVM reserved name
        if (unsafe.count(ext)) {
          foundUnsafe.insert(*it);
        } else {
          klee_warning("undefined reference to %s: %s",
                       it->second ? "variable" : "function", ext.c_str());
        }
      }
    }
  }

  for (std::map<std::string, bool>::iterator it = foundUnsafe.begin(),
                                             ie = foundUnsafe.end();
       it != ie; ++it) {
    const std::string &ext = it->first;
    klee_warning("undefined reference to %s: %s (UNSAFE)!",
                 it->second ? "variable" : "function", ext.c_str());
  }
}

static Interpreter *theInterpreter = 0;

static bool interrupted = false;

static void interrupt_handle() {
  if (!interrupted && theInterpreter) {
    llvm::errs() << "KLEE: ctrl-c detected, requesting interpreter to halt.\n";
    theInterpreter->setHaltExecution(true);
    sys::SetInterruptFunction(interrupt_handle);
  } else {
    llvm::errs() << "KLEE: ctrl-c detected, exiting.\n";
    exit(1);
  }
  interrupted = true;
}

static void replaceOrRenameFunction(llvm::Module *module, const char *old_name,
                                    const char *new_name) {
  Function *new_function, *old_function;
  new_function = module->getFunction(new_name);
  old_function = module->getFunction(old_name);
  if (old_function) {
    if (new_function) {
      old_function->replaceAllUsesWith(new_function);
      old_function->eraseFromParent();
    } else {
      old_function->setName(new_name);
      assert(old_function->getName() == new_name);
    }
  }
}

static void
createLibCWrapper(std::vector<std::unique_ptr<llvm::Module>> &modules,
                  llvm::StringRef intendedFunction,
                  llvm::StringRef libcMainFunction) {
  // We now need to swap things so that libcMainFunction is the entry
  // point, in such a way that the arguments are passed to
  // libcMainFunction correctly. We do this by renaming the user main
  // and generating a stub function to call intendedFunction. There is
  // also an implicit cooperation in that runFunctionAsMain sets up
  // the environment arguments to what a libc expects (following
  // argv), since it does not explicitly take an envp argument.
  auto &ctx = modules[0]->getContext();
  Function *userMainFn = modules[0]->getFunction(intendedFunction);
  assert(userMainFn && "unable to get user main");
  // Rename entry point using a prefix
  userMainFn->setName("__user_" + intendedFunction);

  // force import of libcMainFunction
  llvm::Function *libcMainFn = nullptr;
  for (auto &module : modules) {
    if ((libcMainFn = module->getFunction(libcMainFunction)))
      break;
  }
  if (!libcMainFn)
    klee_error("Could not add %s wrapper", libcMainFunction.str().c_str());

  auto inModuleReference = libcMainFn->getParent()->getOrInsertFunction(
      userMainFn->getName(), userMainFn->getFunctionType());

  const auto ft = libcMainFn->getFunctionType();

  if (ft->getNumParams() != 7)
    klee_error("Imported %s wrapper does not have the correct "
               "number of arguments",
               libcMainFunction.str().c_str());

  std::vector<Type *> fArgs;
  fArgs.push_back(ft->getParamType(1)); // argc
  fArgs.push_back(ft->getParamType(2)); // argv
  Function *stub =
      Function::Create(FunctionType::get(Type::getInt32Ty(ctx), fArgs, false),
                       GlobalVariable::ExternalLinkage, intendedFunction,
                       libcMainFn->getParent());
  BasicBlock *bb = BasicBlock::Create(ctx, "entry", stub);
  llvm::IRBuilder<> Builder(bb);

  std::vector<llvm::Value *> args;
  args.push_back(llvm::ConstantExpr::getBitCast(
      cast<llvm::Constant>(inModuleReference.getCallee()),
      ft->getParamType(0)));
  args.push_back(&*(stub->arg_begin())); // argc
  auto arg_it = stub->arg_begin();
  args.push_back(&*(++arg_it));                                // argv
  args.push_back(Constant::getNullValue(ft->getParamType(3))); // app_init
  args.push_back(Constant::getNullValue(ft->getParamType(4))); // app_fini
  args.push_back(Constant::getNullValue(ft->getParamType(5))); // rtld_fini
  args.push_back(Constant::getNullValue(ft->getParamType(6))); // stack_end
  Builder.CreateCall(libcMainFn, args);
  Builder.CreateUnreachable();
}

static void
linkWithUclibc(StringRef libDir, std::string opt_suffix,
               std::vector<std::unique_ptr<llvm::Module>> &modules) {
  LLVMContext &ctx = modules[0]->getContext();

  size_t newModules = modules.size();

  // Ensure that klee-uclibc exists
  SmallString<128> uclibcBCA(libDir);
  std::string errorMsg;
  llvm::sys::path::append(uclibcBCA, KLEE_UCLIBC_BCA_NAME);
  if (!klee::loadFile(uclibcBCA.c_str(), ctx, modules, errorMsg))
    klee_error("Cannot find klee-uclibc '%s': %s", uclibcBCA.c_str(),
               errorMsg.c_str());

  for (auto i = newModules, j = modules.size(); i < j; ++i) {
    replaceOrRenameFunction(modules[i].get(), "__libc_open", "open");
    replaceOrRenameFunction(modules[i].get(), "__libc_fcntl", "fcntl");
  }

  if (mainFn)
    createLibCWrapper(modules, "main", "__uClibc_main");
  klee_message("NOTE: Using klee-uclibc : %s", uclibcBCA.c_str());

  // Link the fortified library
  SmallString<128> FortifyPath(libDir);
  llvm::sys::path::append(FortifyPath,
                          "libkleeRuntimeFortify" + opt_suffix + ".bca");
  if (!klee::loadFile(FortifyPath.c_str(), ctx, modules, errorMsg))
    klee_error("error loading the fortify library '%s': %s",
               FortifyPath.c_str(), errorMsg.c_str());
}

struct Metaparam {
  std::string name;
  std::vector<llvm::APInt> values;

  void debug() {
    llvm::errs() << "Metaparam: " << name << "\n";
    for (const auto &v : values) {
      llvm::errs() << v << "\n";
    }
  }
};

std::vector<std::vector<llvm::APInt>>
spreadMetaparam(const std::vector<Metaparam> &metaparams) {
  if (metaparams.empty()) {
    return {{}};
  }
  
  std::vector<std::vector<llvm::APInt>> result;

  // Initialize the result with the first metaparam values
  for (const auto &value : metaparams[0].values) {
    result.push_back({value});
  }

  // Iterate through the remaining metaparams and build combinations
  for (size_t i = 1; i < metaparams.size(); ++i) {
    std::vector<std::vector<llvm::APInt>> newResult;
    for (const auto &existingCombination : result) {
      for (const auto &value : metaparams[i].values) {
        auto newCombination = existingCombination;
        newCombination.push_back(value);
        newResult.push_back(std::move(newCombination));
      }
    }
    result = std::move(newResult);
  }

  return result;
}

static LLVMContext ctx;
static Module *mainModule = nullptr;
std::unique_ptr<Interpreter> createInterpreter(char* argv0, KleeHandler* handler) {
  // Load the bytecode...
  std::string errorMsg;
  std::vector<std::unique_ptr<llvm::Module>> loadedModules;

  if (!klee::loadFile(InputFile, ctx, loadedModules, errorMsg)) {
    klee_error("error loading program '%s': %s", InputFile.c_str(),
               errorMsg.c_str());
  }
  // Load and link the whole files content. The assumption is that this is the
  // application under test.
  // Nothing gets removed in the first place.
  std::unique_ptr<llvm::Module> M(klee::linkModules(
      loadedModules, "" /* link all modules together */, errorMsg));
  if (!M) {
    klee_error("error loading program '%s': %s", InputFile.c_str(),
               errorMsg.c_str());
  }

  mainModule = M.get();

  const std::string &module_triple = mainModule->getTargetTriple();
  std::string host_triple = llvm::sys::getDefaultTargetTriple();

  if (module_triple != host_triple)
    klee_warning("Module and host target triples do not match: '%s' != '%s'\n"
                 "This may cause unexpected crashes or assertion violations.",
                 module_triple.c_str(), host_triple.c_str());

  // Detect architecture
  std::string opt_suffix = "64"; // Fall back to 64bit
  if (module_triple.find("i686") != std::string::npos ||
      module_triple.find("i586") != std::string::npos ||
      module_triple.find("i486") != std::string::npos ||
      module_triple.find("i386") != std::string::npos)
    opt_suffix = "32";

  // Add additional user-selected suffix
  opt_suffix += "_" + RuntimeBuild.getValue();

  // Push the module as the first entry
  loadedModules.emplace_back(std::move(M));

  std::string LibraryDir = KleeHandler::getRunTimeLibraryPath(argv0);
  Interpreter::ModuleOptions Opts(LibraryDir.c_str(), EntryPoint, opt_suffix,
                                  /*Optimize=*/false,
                                  /*CheckDivZero=*/CheckDivZero,
                                  /*CheckOvershift=*/CheckOvershift);

  // Get the main function
  for (auto &module : loadedModules) {
    mainFn = module->getFunction("main");
    if (mainFn)
      break;
  }

  // Get the entry point function
  if (EntryPoint.empty())
    klee_error("entry-point cannot be empty");

  for (auto &module : loadedModules) {
    entryFn = module->getFunction(EntryPoint);
    if (entryFn)
      break;
  }

  if (!entryFn)
    klee_error("Entry function '%s' not found in module.", EntryPoint.c_str());

  if (WithPOSIXRuntime) {
    SmallString<128> Path(Opts.LibraryDir);
    llvm::sys::path::append(Path, "libkleeRuntimePOSIX" + opt_suffix + ".bca");
    klee_message("NOTE: Using POSIX model: %s", Path.c_str());
    if (!klee::loadFile(Path.c_str(), mainModule->getContext(), loadedModules,
                        errorMsg))
      klee_error("error loading POSIX support '%s': %s", Path.c_str(),
                 errorMsg.c_str());

    std::string libcPrefix = (Libc == LibcType::UcLibc ? "__user_" : "");
    if (mainFn)
      preparePOSIX(loadedModules, libcPrefix);
  }

  if (WithUBSanRuntime) {
    SmallString<128> Path(Opts.LibraryDir);
    llvm::sys::path::append(Path, "libkleeUBSan" + opt_suffix + ".bca");
    if (!klee::loadFile(Path.c_str(), mainModule->getContext(), loadedModules,
                        errorMsg))
      klee_error("error loading UBSan support '%s': %s", Path.c_str(),
                 errorMsg.c_str());
  }

  switch (Libc) {
  case LibcType::KleeLibc: {
    // FIXME: Find a reasonable solution for this.
    SmallString<128> Path(Opts.LibraryDir);
    llvm::sys::path::append(Path,
                            "libkleeRuntimeKLEELibc" + opt_suffix + ".bca");
    if (!klee::loadFile(Path.c_str(), mainModule->getContext(), loadedModules,
                        errorMsg))
      klee_error("error loading klee libc '%s': %s", Path.c_str(),
                 errorMsg.c_str());
  }
  /* Falls through. */
  case LibcType::FreestandingLibc: {
    SmallString<128> Path(Opts.LibraryDir);
    llvm::sys::path::append(Path,
                            "libkleeRuntimeFreestanding" + opt_suffix + ".bca");
    if (!klee::loadFile(Path.c_str(), mainModule->getContext(), loadedModules,
                        errorMsg))
      klee_error("error loading freestanding support '%s': %s", Path.c_str(),
                 errorMsg.c_str());
    break;
  }
  case LibcType::UcLibc:
    linkWithUclibc(LibraryDir, opt_suffix, loadedModules);
    break;
  }

  for (const auto &library : LinkLibraries) {
    if (!klee::loadFile(library, mainModule->getContext(), loadedModules,
                        errorMsg))
      klee_error("error loading bitcode library '%s': %s", library.c_str(),
                 errorMsg.c_str());
  }

  Interpreter::InterpreterOptions IOpts;
  IOpts.MakeConcreteSymbolic = 0;
  Interpreter *interpreter = theInterpreter =
      Interpreter::create(ctx, IOpts, handler);
  assert(interpreter);
  interpreter->setInhibitForking(true);
  handler->setInterpreter(interpreter);

  auto finalModule = interpreter->setModule(loadedModules, Opts);
  entryFn = finalModule->getFunction(EntryPoint);
  if (!entryFn)
    klee_error("Entry function '%s' not found in module.", EntryPoint.c_str());

  externalsAndGlobalsCheck(finalModule);

  return std::unique_ptr<Interpreter>(interpreter);
}

int main(int argc, char **argv, char **envp) {
  atexit(llvm_shutdown); // Call llvm_shutdown() on exit

  KCommandLine::KeepOnlyCategories(
      {&ChecksCat, &DebugCat, &ExtCallsCat, &ExprCat, &LinkCat, &MemoryCat,
       &MergeCat, &MiscCat, &ModuleCat, &SearchCat, &SeedingCat, &SolvingCat,
       &StartCat, &StatsCat, &TerminationCat, &TestGenCat, &ExecTreeCat,
       &ExecTreeCat});
  llvm::InitializeNativeTarget();

  parseArguments(argc, argv);
  sys::PrintStackTraceOnErrorSignal(argv[0]);

  sys::SetInterruptFunction(interrupt_handle);

  KleeHandler *handler = new KleeHandler();
  auto interpreter = createInterpreter(argv[0], handler);

  for (int i = 0; i < argc; i++)
    handler->getInfoStream() << argv[i] << (i + 1 < argc ? " " : "\n");
  handler->getInfoStream() << "PID: " << getpid() << "\n";

  // Get the desired main function.  klee_main initializes uClibc
  // locale and other data and then calls main.

  std::vector<Metaparam> metaparams;
  if (entryFn->arg_size() > 0) {
    if (auto metaparam =
            entryFn->getMetadata(llvm::LLVMContext::MD_bonc_metaparam)) {
      for (const auto &i : metaparam->operands()) {
        if (auto arg = dyn_cast<MDNode>(i.get())) {
          if (arg->getNumOperands() != 2) {
            klee_error("Malformed [[bonc::metaparam]], should be {string, "
                       "node-of-ints}");
          }
          auto name = dyn_cast<MDString>(arg->getOperand(0));
          if (!name) {
            klee_error("Malformed [[bonc::metaparam]], first operand should be "
                       "a string");
          }
          auto name_str = name->getString().str();
          auto values = dyn_cast<MDNode>(arg->getOperand(1));
          if (!values) {
            klee_error("Malformed [[bonc::metaparam]], second operand should "
                       "be a node-of-ints");
          }
          if (values->getNumOperands() == 0) {
            klee_error("[[bonc::metaparam]] of '%s' has no values",
                       name_str.c_str());
          }
          std::vector<llvm::APInt> values_vec;
          for (const auto &v : values->operands()) {
            auto i = dyn_cast<ConstantAsMetadata>(v.get());
            if (!i) {
              klee_error("Malformed [[bonc::metaparam]], second operand should "
                         "be a node-of-ints");
            }
            auto int_val = dyn_cast<ConstantInt>(i->getValue());
            if (int_val) {
              values_vec.push_back(int_val->getValue());
            } else {
              klee_error("Malformed [[bonc::metaparam]], second operand should "
                         "be a node-of-ints");
            }
          }
          metaparams.push_back({name_str, std::move(values_vec)});
        } else {
          i->printAsOperand(llvm::errs());
          klee_error("Unexpected metadata type for bonc::metaparam");
        }
      }
    } else {
      klee_error("Entry function '%s' contains "
                 "non-[[bonc::metaparam]]-annotated parameters.",
                 EntryPoint.c_str());
    }
  }

  auto all_metaparam_combinations = spreadMetaparam(metaparams);
  if (all_metaparam_combinations.empty()) {
    klee_error("No metaparam values found");
  }
  if (all_metaparam_combinations.size() > 65536) {
    klee_error("Too many metaparam combinations found: %zu",
               all_metaparam_combinations.size());
  }
  klee_message("Found %zu metaparam combinations",
               all_metaparam_combinations.size());
  for (const auto &combination : all_metaparam_combinations) {
    std::stringstream name_ss;
    std::vector<klee::ref<klee::Expr>> args;
    for (size_t i = 0; i < combination.size(); ++i) {
      if (i > 0) {
        name_ss << "_";
      }
      name_ss << metaparams.at(i).name << combination.at(i).getSExtValue();

      args.push_back(klee::ConstantExpr::create(
          combination.at(i).getZExtValue(),
          entryFn->getArg(i)->getType()->getIntegerBitWidth()));
    }
    auto name = name_ss.str();
    klee_message("Running with metaparam combination: %s", name.c_str());

    interpreter.reset();
    interpreter = createInterpreter(argv[0], handler);
    
    interpreter->runFunction(entryFn, args);
    std::string error;
    auto out_path = handler->getOutputFilename("bonc_" + name + ".json");
    auto f = klee_open_output_file(out_path, error);
    if (!f) {
      klee_warning("error opening file \"%s\" (%s).", out_path.c_str(),
                   error.c_str());
      return 1;
    }
    interpreter->printBoncResult(*f);
  }
  
  delete handler;

  return 0;
}
