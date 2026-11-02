// utils/logger.h
#ifndef GRAPHFLOW_LOGGER_H
#define GRAPHFLOW_LOGGER_H

#include <sstream>
#include "utoolkit/logging/logger.h"

#ifndef UT_LOG_TAG
#define UT_LOG_TAG "GraphFlow"
#endif

#if defined(ANDROID)
#include <android/log.h>
#define LOG_DEBUG(tag, msg)                                                      \
  {                                                                              \
    std::stringstream ss;                                                        \
    ss << "[" << tag << "] [" << __FUNCTION__ << ":" << __LINE__ << "] " << msg; \
    __android_log_print(ANDROID_LOG_DEBUG, UT_LOG_TAG, "%s", ss.str().c_str());  \
  }
#define LOG_INFO(tag, msg)                                                       \
  {                                                                              \
    std::stringstream ss;                                                        \
    ss << "[" << tag << "] [" << __FUNCTION__ << ":" << __LINE__ << "] " << msg; \
    __android_log_print(ANDROID_LOG_INFO, UT_LOG_TAG, "%s", ss.str().c_str());   \
  }
#define LOG_WARN(tag, msg)                                                       \
  {                                                                              \
    std::stringstream ss;                                                        \
    ss << "[" << tag << "] [" << __FUNCTION__ << ":" << __LINE__ << "] " << msg; \
    __android_log_print(ANDROID_LOG_WARN, UT_LOG_TAG, "%s", ss.str().c_str());   \
  }
#define LOG_ERROR(tag, msg)                                                      \
  {                                                                              \
    std::stringstream ss;                                                        \
    ss << "[" << tag << "] [" << __FUNCTION__ << ":" << __LINE__ << "] " << msg; \
    __android_log_print(ANDROID_LOG_ERROR, UT_LOG_TAG, "%s", ss.str().c_str());  \
  }
#define LOG_CRITICAL(tag, msg)                                                   \
  {                                                                              \
    std::stringstream ss;                                                        \
    ss << "[" << tag << "] [" << __FUNCTION__ << ":" << __LINE__ << "] " << msg; \
    __android_log_print(ANDROID_LOG_FATAL, UT_LOG_TAG, "%s", ss.str().c_str());  \
  }
#else
#define LOG_DEBUG(tag, msg)                                                      \
  {                                                                              \
    std::stringstream ss;                                                        \
    ss << "[" << tag << "] [" << __FUNCTION__ << ":" << __LINE__ << "] " << msg; \
    utoolkit::logging::Logger::GetInstance().Debug(ss.str().c_str());            \
  }
#define LOG_INFO(tag, msg)                                                       \
  {                                                                              \
    std::stringstream ss;                                                        \
    ss << "[" << tag << "] [" << __FUNCTION__ << ":" << __LINE__ << "] " << msg; \
    utoolkit::logging::Logger::GetInstance().Info(ss.str().c_str());             \
  }
#define LOG_WARN(tag, msg)                                                       \
  {                                                                              \
    std::stringstream ss;                                                        \
    ss << "[" << tag << "] [" << __FUNCTION__ << ":" << __LINE__ << "] " << msg; \
    utoolkit::logging::Logger::GetInstance().Warn(ss.str().c_str());             \
  }
#define LOG_ERROR(tag, msg)                                                      \
  {                                                                              \
    std::stringstream ss;                                                        \
    ss << "[" << tag << "] [" << __FUNCTION__ << ":" << __LINE__ << "] " << msg; \
    utoolkit::logging::Logger::GetInstance().Error(ss.str().c_str());            \
  }
#define LOG_CRITICAL(tag, msg)                                                   \
  {                                                                              \
    std::stringstream ss;                                                        \
    ss << "[" << tag << "] [" << __FUNCTION__ << ":" << __LINE__ << "] " << msg; \
    utoolkit::logging::Logger::GetInstance().Critical(ss.str().c_str());         \
  }
#endif  // ANDROID

#endif  // GRAPHFLOW_LOGGER_H
