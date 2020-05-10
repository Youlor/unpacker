#ifndef ART_RUNTIME_UNPACKER_H_
#define ART_RUNTIME_UNPACKER_H_

#include "jni_internal.h"
#include "base/mutex.h"
#include "mirror/class.h"
#include <list>
#include "cJSON.h"

namespace art{

class ArtMethod;
class Thread;

class Unpacker {
private:
  static bool dumping_method_;
  static Thread* self_;
  static std::string dump_dir_;
  static std::string dex_dir_;
  static std::string method_dir_;
  static std::string json_path_;
  static int json_fd_;
  static cJSON* json_;
  static std::list<const DexFile*> dex_files_;
  static mirror::ClassLoader* class_loader_;
  static std::map<std::string, int> method_fds_;

  //获取dump目录
  static std::string getDumpDir();
  //获取dex dump路径
  static std::string getDexDumpPath(const DexFile* dex_file);
  //获取method dump路径
  static std::string getMethodDumpPath(ArtMethod* method) SHARED_REQUIRES(Locks::mutator_lock_);
  //创建json
  static cJSON* createJson();
  //读取并解析json
  static cJSON* parseJson();
  //写入json
  static void writeJson();

  //获取所有的DexFile
  static std::list<const DexFile*> getDexFiles() SHARED_REQUIRES(Locks::mutator_lock_);
  //获取App的ClassLoader
  static mirror::ClassLoader* getAppClassLoader() SHARED_REQUIRES(Locks::mutator_lock_);
  //获取method code item size
  static size_t getCodeItemSize(ArtMethod* method) SHARED_REQUIRES(Locks::mutator_lock_);

  //初始化
  static void init() SHARED_REQUIRES(Locks::mutator_lock_);
  //解析所有dexfile所有引用的类
  static void resolveAllTypes() SHARED_REQUIRES(Locks::mutator_lock_);
  //主动调用所有方法
  static void invokeAllMethods() SHARED_REQUIRES(Locks::mutator_lock_);
  //dump dex
  static void dumpAllDexes();
  //还原
  static void fini();
  
public:
  //脱壳!
  static void unpack();
  //方法是否需要解释器执行: 主动调用的方法需要通过Switch型解释器执行
  static bool shouldInterpreter(Thread *self, ArtMethod *method) SHARED_REQUIRES(Locks::mutator_lock_);
  //dump method: 在每条指令解释执行时调用该方法来dump method codeitem
  static bool dumpMethod(Thread *self, ArtMethod *method, uint32_t dex_pc) SHARED_REQUIRES(Locks::mutator_lock_);
  //动态注册native方法
  static void register_cn_youlor_Unpacker(JNIEnv* env);
};

}

#endif