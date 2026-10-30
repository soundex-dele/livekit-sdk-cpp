# Android JNI 完成回调与持续 Observer

当前仓库没有 Android 构建模块。本文是绑定设计与示意代码，不是已编译或已在 Android 设备验收的实现。
C++ 接口以 [Completion/Observer 契约](rtc-observers.md) 为准。

## Java 接口

```java
interface Completion<T> {
    void onComplete(T value, RtcError error);
}

// 返回受理错误码；0 为已受理，最终结果走 completion。
static native int nativeJoin(long engine, RoomOptions options, Completion<Void> completion);
static native int nativePublishMedia(long engine, MediaConfig config,
                                    Completion<MediaPublication> completion);
static native int nativeClose(long engine, Completion<Void> completion);
static native void nativeRelease(long engine);
```

受理失败不调用 completion；已受理操作在原生串行通知线程调用一次 completion。
close 允许不提供 completion，不依赖已注册 Observer。持续房间和轨道事件单独注册 Observer，
不会再通过 Observer 派发 join/publish 等单次结果。没有操作句柄或请求 ID 映射表。
Java 包装层可以将受理失败也转成回调，但必须仅在非零返回时转换，避免重复完成。

## JNI 入口结构

以下 JniCompletion、acquire_engine、read_config、report_native_exception 是绑定层需要实现的辅助类型，
不是 SDK 已提供的函数。句柄表返回受管引用，不能将 Long 直接强转为可删除的裸指针。

```cpp
jint native_publish_media(JNIEnv* env, jclass, jlong handle,
                          jobject config, jobject callback) {
  try {
    auto engine = acquire_engine(handle);
    auto spec = read_config(env, config);
    if (env->ExceptionCheck()) return 0;  // JVM 将传播挂起异常，返回值不被消费。
    auto completion = JniCompletion::create(env, callback);  // NewGlobalRef
    if (env->ExceptionCheck()) return 0;
    auto code = engine->publish_media(std::move(spec),
        [completion](Result<MediaPublication> result) {
          completion->complete(std::move(result));
        });
    return android_error_code(code);
  } catch (...) {
    return report_native_exception(env);  // 不允许 C++ 异常跨 JNI；保留已有 JVM 异常。
  }
}
```

- JniCompletion 保存 JavaVM 和 GlobalRef，不保存跨线程复用的 JNIEnv。
- complete 获取当前线程 JNIEnv，必要时 AttachCurrentThread。只有自行附加的线程才由桥接层 detach。
- Result<T>/Result<void> 转换成独立 Java 结果对象，保留 code、message、domain、failure id 和上下文。
  不向 Java 暴露 C++ 模板或临时内存地址。MediaPublication 中的原生对象使用独立受管句柄。
- 受理失败时本地 completion 引用释放；成功后 SDK 持有其捕获，完成任务释放时 DeleteGlobalRef。
  取消、关闭、分配失败和通知任务销毁路径都须释放引用，不能因界面销毁提前释放仍可能使用的引用。
- Java 回调抛异常时，在该异步线程记录并处理挂起异常，不能重试回调或带着异常继续 JNI 调用。
- nativeRelease 先从句柄表撤销访问，再在释放线程等待已有调用并完成最终析构。
  受管对象的最终 deleter 也必须使用释放线程，不能让最后一个引用在 SDK 通知线程触发同步析构。

## Java Executor 和协程

Java 包装层可以选择 UI 或后台 Executor：

```java
return nativePublishMedia(handle, config, (value, error) -> {
    executor.execute(() -> completion.onComplete(value, error));
});
```

Executor 拒绝任务时必须报告绑定层派发失败并释放结果句柄；不要切换到原生控制线程补发。
需要严格顺序时使用串行 Executor。原生 close 完成不代表此前转发的 UI 任务已经执行，
界面层可使用自己的会话代次丢弃过期结果。

原生回调中可以提交下一次异步请求或 close，不得等待 SDK 工作、同步等待 UI 或直接销毁 Engine。
JNI release 必须非阻塞转交给释放线程。Kotlin suspend 包装可使用 suspendCancellableCoroutine；
协程取消只取消业务等待，不代表远端操作已经撤销，仍由原生终态回调释放桥接资源。

## 固定错误码映射

绑定层必须使用显式 switch；不使用 `static_cast<jint>(code)`，也不依赖 Kotlin enum ordinal。以下为建议的初版映射，采用后应作为绑定协议固定，并同步维护 Kotlin 常量；新增 SDK 错误码在映射更新前落到 INTERNAL_ERROR。

```cpp
jint android_error_code(livekit::sdk::ErrorCode code) noexcept {
  using livekit::sdk::ErrorCode;
  switch (code) {
    case ErrorCode::ok: return 0;
    case ErrorCode::connection_lost: return 1001;
    case ErrorCode::timeout: return 1002;
    case ErrorCode::cancelled: return 1003;
    case ErrorCode::permission_denied: return 1004;
    case ErrorCode::malformed_message: return 1005;
    case ErrorCode::resource_exhausted: return 1006;
    case ErrorCode::stale_handle: return 1007;
    case ErrorCode::session_closed: return 1008;
    case ErrorCode::recovery_failed: return 1009;
    case ErrorCode::unsupported: return 1010;
    case ErrorCode::outcome_unknown: return 1011;
    case ErrorCode::request_failed: return 1012;
    case ErrorCode::invalid_argument: return 1013;
    case ErrorCode::processing_failed: return 1014;
    case ErrorCode::callback_failed: return 1015;
    case ErrorCode::invalid_thread: return 1016;
    case ErrorCode::invalid_state: return 1017;
    case ErrorCode::busy: return 1018;
    case ErrorCode::internal_error: return 1099;
  }
  return 1099;
}
```

## 字符串、类加载和验证

一般 UTF-8 不等于 JNI Modified UTF-8。原生字符串转换为 UTF-16 后使用 NewString，覆盖中文、emoji
和内嵌 NUL；Java 输入同样明确 UTF-16/UTF-8 转换及长度上限。缓存的 jclass 使用 GlobalRef，
在正确的应用 ClassLoader 上初始化，并配套 R8/ProGuard keep 规则。

Android 接入需要独立验证：受理失败、成功值、完整错误、回调先于 native 方法返回、回调内 close、
并发 release、GlobalRef 回收、Executor 拒绝、协程取消、回调异常、非 ASCII 字符串及 R8 构建。
C++ 无窗口测试不能替代这些绑定和设备验证。
