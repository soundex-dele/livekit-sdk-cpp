# Android JNI 结果转换示例

当前仓库没有 Android 构建模块。本文是接入示例与边界约定，不是已经编译或在 Android 设备验收的绑定实现。

## C++ 与 Android 的职责

C++ 无数据控制操作返回 `ErrorCode`；可能失败的有值操作返回 `Result<T>`。JNI 负责把后者转换成 Kotlin/Java 对象，不向 Java 暴露 C++ 模板、对象地址或错误码输出指针。同步 SDK 控制仍在调用线程执行。

| C++ 接口 | Android 表达 |
| --- | --- |
| `ErrorCode close()` | `Int`，零表示成功 |
| `Result<TrackId> publish(...)` | `PublishResult`，成功包含 trackId，失败包含 code/message |
| `Result<std::unique_ptr<RtcEngine>>` | 注册到 native 句柄表，返回不透明的 Long 令牌及失败结果 |
| `FailureInfo` | 按需要复制 code、message、id、domain、retryable、backend_reason 与 context |

值结果在 JNI 转换之前一直保留完整 `FailureInfo`。业务失败通过结果返回；Java 对象分配失败等 JVM 异常保留为 Java 异常。不要用空 trackId 同时表示失败和合法值。

## Kotlin 调用示例

```kotlin
package com.example.rtc

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

// 示例值是绑定层协议，不是 C++ enum 的 ordinal。
object RtcCode {
    const val OK = 0
    const val TIMEOUT = 1002
    const val PERMISSION_DENIED = 1004
    const val INVALID_ARGUMENT = 1013
    const val INTERNAL_ERROR = 1099
}

data class PublishResult(
    val code: Int,
    val trackId: String?,
    val message: String?
)

object NativeRtc {
    // 库加载、NativeTrackDescriptor 和句柄创建由 Android 项目实现。
    external fun publish(handle: Long, descriptor: NativeTrackDescriptor): PublishResult
    external fun close(handle: Long): Int
}

suspend fun publishTrack(handle: Long, descriptor: NativeTrackDescriptor): PublishResult =
    withContext(Dispatchers.IO) {
        NativeRtc.publish(handle, descriptor)
    }
```

调用方先判断 `code == RtcCode.OK`，成功时再读取非空 `trackId`；失败时展示 `message`。结果类和 native 方法需要配套 R8/ProGuard keep 规则，避免 JNI 构造函数或注册名称被改写。

## JNI 入口示例

下列辅助函数由 Android 项目实现，并不是 SDK 已提供的函数：

- `acquire_engine(handle)`：从线程安全句柄表取得 `shared_ptr<RtcEngine>`，让引擎在本次调用结束前保持存活；无效或已释放令牌转换为 `stale_handle`。
- `read_descriptor(env, descriptor)`：读取 Java 字段、复制成原生 `TrackDescriptor`，校验参数；失败时设置 Java 异常或抛出 SDKError。
- `make_publish_result(...) noexcept`：构造 `(ILjava/lang/String;Ljava/lang/String;)V` 的 Java 结果对象；失败返回 nullptr，并保证已有 Java 异常挂起。复制字符串内容，不保存原生字符串指针。
- `report_native_exception(env) noexcept`：仅在尚无 Java 异常时设置 Java 异常；包括内存分配失败在内，不能再让 C++ 异常逃出。

```cpp
extern "C" JNIEXPORT jobject JNICALL
Java_com_example_rtc_NativeRtc_publish(JNIEnv* env, jobject, jlong handle,
                                     jobject descriptor) {
  try {
    auto engine = acquire_engine(handle);
    auto native_descriptor = read_descriptor(env, descriptor);
    if (env->ExceptionCheck()) {
      return nullptr;
    }
    auto result = engine->publish(std::move(native_descriptor));
    if (!result) {
      const auto& error = result.error();
      return make_publish_result(env, android_error_code(error.code),
                                 nullptr, &error.message);
    }
    return make_publish_result(env, 0, &*result, nullptr);
  } catch (const livekit::sdk::SDKError& error) {
    if (env->ExceptionCheck()) {
      return nullptr;
    }
    return make_publish_result(env, android_error_code(error.code()),
                               nullptr, &error.failure()->message);
  } catch (...) {
    report_native_exception(env);
    return nullptr;
  }
}
```

无返回数据的 JNI 控制入口同样取得引擎存活引用、检查挂起异常，并捕获所有 C++ 异常；正常返回 `android_error_code(engine->close())`。`close` 只关闭会话；释放句柄表令牌应使用单独的 `release` 接口，拒绝新调用并让已开始的调用自然完成。不能直接把 `jlong` 强转成裸指针后删除。

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
    case ErrorCode::internal_error: return 1099;
  }
  return 1099;
}
```

## 线程、字符串和生命周期

- `publish/join/close` 可能等待协议确认或资源清理，使用后台 dispatcher，不能阻塞 Android 主线程，也不能在 SDK 媒体或通知回调中重入控制。
- 取消 Kotlin 协程不等于取消正在执行的 JNI 调用。`withContext(Dispatchers.IO)` 只切换执行线程；调用期间仍要保留引擎，不能因协程取消销毁 native 对象。
- `JNIEnv*` 只属于当前线程。缓存 JavaVM，原生回调线程 attach 后取得自己的 JNIEnv；跨线程保存 Java 对象要用 GlobalRef，按明确的订阅生命周期释放。
- `FailureInfo.message` 和 TrackId 是原生字符串。一般 UTF-8 不等于 JNI Modified UTF-8；中文、emoji、内嵌 NUL 应解码为 UTF-16，再使用 `NewString`。从 Java 传入时同样明确 UTF-16/UTF-8 转换与字段长度限制。
- 检查 JNI 分配、字段读取及对象构造的挂起异常；及时释放局部引用。缓存的 jclass 使用 GlobalRef，并由 Android 应用类加载器正确初始化。
- 实际 Android 接入应验证成功值、完整错误元数据、超时、无效句柄、并发 release、协程取消、非 ASCII 字符串及 R8 构建。当前 C++ 无窗口测试不代表这些 JNI/设备路径已通过验收。
