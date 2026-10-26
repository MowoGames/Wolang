// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/tls.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <windows.h>

#include <schannel.h>
#include <security.h>
#include <sspi.h>
#else
#include <dlfcn.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "wolang/runtime/sockets.h"

namespace wolang {
namespace runtime {

#if defined(_WIN32)

static bool SendAll(int64_t socket, const uint8_t* bytes, int64_t count) {
  int64_t sent = 0;
  while (sent < count) {
    const int64_t written = wolang_socket_send(socket, bytes + sent, count - sent);
    if (written <= 0) {
      return false;
    }
    sent += written;
  }
  return true;
}

struct Session {
  int64_t Socket = -1;
  CredHandle Credentials = {};
  CtxtHandle Context = {};
  bool HasContext = false;
  SecPkgContext_StreamSizes Sizes = {};
  std::vector<uint8_t> Incoming;
  std::vector<uint8_t> Plain;
  size_t PlainAt = 0;
  bool Closed = false;
};

static constexpr DWORD kRequest = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_USE_SUPPLIED_CREDS;

static bool Receive(Session* session) {
  uint8_t chunk[16384];
  const int64_t got = wolang_socket_receive(session->Socket, chunk, sizeof(chunk));
  if (got <= 0) {
    return false;
  }

  session->Incoming.insert(session->Incoming.end(), chunk, chunk + got);
  return true;
}

static bool SendTokens(Session* session, SecBufferDesc* out) {
  bool sent = true;
  for (ULONG i = 0; i < out->cBuffers; ++i) {
    SecBuffer& buffer = out->pBuffers[i];
    if (buffer.BufferType == SECBUFFER_TOKEN && buffer.cbBuffer != 0 && buffer.pvBuffer != nullptr) {
      sent = sent && SendAll(session->Socket, static_cast<const uint8_t*>(buffer.pvBuffer), buffer.cbBuffer);
    }
    if (buffer.pvBuffer != nullptr) {
      ::FreeContextBuffer(buffer.pvBuffer);
      buffer.pvBuffer = nullptr;
    }
  }
  return sent;
}

static bool Shake(Session* session, const std::wstring& host, bool first) {
  for (;;) {
    SecBuffer in[2] = {};
    in[0].BufferType = SECBUFFER_TOKEN;
    in[0].pvBuffer = session->Incoming.data();
    in[0].cbBuffer = static_cast<ULONG>(session->Incoming.size());
    in[1].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc in_desc = {SECBUFFER_VERSION, 2, in};

    SecBuffer out[1] = {};
    out[0].BufferType = SECBUFFER_TOKEN;
    SecBufferDesc out_desc = {SECBUFFER_VERSION, 1, out};

    ULONG attributes = 0;
    const SECURITY_STATUS status = ::InitializeSecurityContextW(&session->Credentials, first ? nullptr : &session->Context, const_cast<wchar_t*>(host.c_str()), kRequest, 0, 0, first ? nullptr : &in_desc, 0, &session->Context, &out_desc, &attributes, nullptr);
    session->HasContext = true;
    first = false;

    if (status == SEC_E_INCOMPLETE_MESSAGE) {
      if (!Receive(session)) {
        return false;
      }
      continue;
    }
    if (!SendTokens(session, &out_desc)) {
      return false;
    }
    if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) {
      return false;
    }

    if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer != 0) {
      session->Incoming.erase(session->Incoming.begin(), session->Incoming.end() - in[1].cbBuffer);
    }
    else if (!session->Incoming.empty() || status == SEC_I_CONTINUE_NEEDED) {
      session->Incoming.clear();
    }

    if (status == SEC_E_OK) {
      return true;
    }
    if (!Receive(session)) {
      return false;
    }
  }
}

static void Destroy(Session* session) {
  if (session->HasContext) {
    ::DeleteSecurityContext(&session->Context);
  }
  ::FreeCredentialsHandle(&session->Credentials);
  delete session;
}

extern "C" int64_t wolang_tls_open(int64_t socket, const uint8_t* host, uint64_t length, int32_t verify) {
  auto* session = new Session();
  session->Socket = socket;

  SCHANNEL_CRED credentials = {};
  credentials.dwVersion = SCHANNEL_CRED_VERSION;
  credentials.dwFlags = SCH_USE_STRONG_CRYPTO | (verify != 0 ? SCH_CRED_AUTO_CRED_VALIDATION : SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_SERVERNAME_CHECK);
  if (::AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &credentials, nullptr, nullptr, &session->Credentials, nullptr) != SEC_E_OK) {
    delete session;
    return 0;
  }

  const std::string narrow(reinterpret_cast<const char*>(host), static_cast<size_t>(length));
  const std::wstring wide(narrow.begin(), narrow.end());
  if (!Shake(session, wide, true) || ::QueryContextAttributesW(&session->Context, SECPKG_ATTR_STREAM_SIZES, &session->Sizes) != SEC_E_OK) {
    Destroy(session);
    return 0;
  }
  return reinterpret_cast<int64_t>(session);
}

extern "C" int64_t wolang_tls_write(int64_t handle, const uint8_t* bytes, int64_t count) {
  auto* session = reinterpret_cast<Session*>(handle);
  if (session == nullptr || session->Closed) {
    return -1;
  }

  const SecPkgContext_StreamSizes& sizes = session->Sizes;
  std::vector<uint8_t> message(sizes.cbHeader + sizes.cbMaximumMessage + sizes.cbTrailer);
  int64_t done = 0;
  while (done < count) {
    const ULONG piece = static_cast<ULONG>(count - done < sizes.cbMaximumMessage ? count - done : sizes.cbMaximumMessage);
    std::memcpy(message.data() + sizes.cbHeader, bytes + done, piece);

    SecBuffer buffers[4] = {};
    buffers[0] = {sizes.cbHeader, SECBUFFER_STREAM_HEADER, message.data()};
    buffers[1] = {piece, SECBUFFER_DATA, message.data() + sizes.cbHeader};
    buffers[2] = {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, message.data() + sizes.cbHeader + piece};
    buffers[3] = {0, SECBUFFER_EMPTY, nullptr};
    SecBufferDesc desc = {SECBUFFER_VERSION, 4, buffers};
    if (::EncryptMessage(&session->Context, 0, &desc, 0) != SEC_E_OK) {
      return -1;
    }
    if (!SendAll(session->Socket, message.data(), buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer)) {
      return -1;
    }
    done += piece;
  }
  return done;
}

extern "C" int64_t wolang_tls_read(int64_t handle, uint8_t* bytes, int64_t room) {
  auto* session = reinterpret_cast<Session*>(handle);
  if (session == nullptr || room <= 0) {
    return -1;
  }

  while (session->PlainAt == session->Plain.size()) {
    session->Plain.clear();
    session->PlainAt = 0;
    if (session->Closed) {
      return 0;
    }
    if (session->Incoming.empty() && !Receive(session)) {
      return 0;
    }

    SecBuffer buffers[4] = {};
    buffers[0] = {static_cast<ULONG>(session->Incoming.size()), SECBUFFER_DATA, session->Incoming.data()};
    buffers[1].BufferType = SECBUFFER_EMPTY;
    buffers[2].BufferType = SECBUFFER_EMPTY;
    buffers[3].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc desc = {SECBUFFER_VERSION, 4, buffers};
    const SECURITY_STATUS status = ::DecryptMessage(&session->Context, &desc, 0, nullptr);
    if (status == SEC_E_INCOMPLETE_MESSAGE) {
      if (!Receive(session)) {
        return 0;
      }
      continue;
    }
    if (status == SEC_I_CONTEXT_EXPIRED) {
      session->Closed = true;
      return 0;
    }
    if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) {
      return -1;
    }

    std::vector<uint8_t> extra;
    for (SecBuffer& buffer : buffers) {
      if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer != 0) {
        const auto* data = static_cast<const uint8_t*>(buffer.pvBuffer);
        session->Plain.insert(session->Plain.end(), data, data + buffer.cbBuffer);
      }
      if (buffer.BufferType == SECBUFFER_EXTRA && buffer.cbBuffer != 0) {
        const auto* data = static_cast<const uint8_t*>(buffer.pvBuffer);
        extra.assign(data, data + buffer.cbBuffer);
      }
    }
    session->Incoming = std::move(extra);

    if (status == SEC_I_RENEGOTIATE) {
      const std::wstring none;
      if (!Shake(session, none, false)) {
        return -1;
      }
    }
  }

  const size_t left = session->Plain.size() - session->PlainAt;
  const size_t take = left < static_cast<size_t>(room) ? left : static_cast<size_t>(room);
  std::memcpy(bytes, session->Plain.data() + session->PlainAt, take);
  session->PlainAt += take;
  return static_cast<int64_t>(take);
}

extern "C" void wolang_tls_close(int64_t handle) {
  auto* session = reinterpret_cast<Session*>(handle);
  if (session == nullptr) {
    return;
  }

  DWORD kind = SCHANNEL_SHUTDOWN;
  SecBuffer token = {sizeof(kind), SECBUFFER_TOKEN, &kind};
  SecBufferDesc token_desc = {SECBUFFER_VERSION, 1, &token};
  if (session->HasContext && ::ApplyControlToken(&session->Context, &token_desc) == SEC_E_OK) {
    SecBuffer out[1] = {};
    out[0].BufferType = SECBUFFER_TOKEN;
    SecBufferDesc out_desc = {SECBUFFER_VERSION, 1, out};
    ULONG attributes = 0;
    if (::InitializeSecurityContextW(&session->Credentials, &session->Context, nullptr, kRequest, 0, 0, nullptr, 0, &session->Context, &out_desc, &attributes, nullptr) == SEC_E_OK) {
      SendTokens(session, &out_desc);
    }
  }
  Destroy(session);
}

#else

struct OpenSsl {
  void* (*ClientMethod)() = nullptr;
  void* (*ContextNew)(void*) = nullptr;
  void (*ContextFree)(void*) = nullptr;
  void (*SetVerify)(void*, int, void*) = nullptr;
  int (*DefaultPaths)(void*) = nullptr;
  void* (*New)(void*) = nullptr;
  void (*Free)(void*) = nullptr;
  int (*SetFd)(void*, int) = nullptr;
  int (*Connect)(void*) = nullptr;
  int (*Read)(void*, void*, int) = nullptr;
  int (*Write)(void*, const void*, int) = nullptr;
  int (*Shutdown)(void*) = nullptr;
  int64_t (*VerifyResult)(const void*) = nullptr;
  int64_t (*Control)(void*, int, int64_t, void*) = nullptr;
  int (*SetHost)(void*, const char*) = nullptr;
};

template <typename T>
static bool Bind(void* library, const char* name, T* into) {
  *into = reinterpret_cast<T>(::dlsym(library, name));
  return *into != nullptr;
}

static const OpenSsl* LoadOpenSsl() {
  static const OpenSsl* loaded = []() -> const OpenSsl* {
    static const char* kNames[] = {"libssl.so.3", "libssl.so", "/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib", "/usr/local/opt/openssl@3/lib/libssl.3.dylib", "libssl.3.dylib"};
    void* library = nullptr;
    for (const char* name : kNames) {
      library = ::dlopen(name, RTLD_NOW | RTLD_GLOBAL);
      if (library != nullptr) {
        break;
      }
    }
    if (library == nullptr) {
      return nullptr;
    }

    static OpenSsl ssl;
    const bool bound = Bind(library, "TLS_client_method", &ssl.ClientMethod) && Bind(library, "SSL_CTX_new", &ssl.ContextNew) && Bind(library, "SSL_CTX_free", &ssl.ContextFree) && Bind(library, "SSL_CTX_set_verify", &ssl.SetVerify) && Bind(library, "SSL_CTX_set_default_verify_paths", &ssl.DefaultPaths) && Bind(library, "SSL_new", &ssl.New) && Bind(library, "SSL_free", &ssl.Free) && Bind(library, "SSL_set_fd", &ssl.SetFd) && Bind(library, "SSL_connect", &ssl.Connect) && Bind(library, "SSL_read", &ssl.Read) && Bind(library, "SSL_write", &ssl.Write) && Bind(library, "SSL_shutdown", &ssl.Shutdown) && Bind(library, "SSL_get_verify_result", &ssl.VerifyResult) && Bind(library, "SSL_ctrl", &ssl.Control) && Bind(library, "SSL_set1_host", &ssl.SetHost);
    return bound ? &ssl : nullptr;
  }();
  return loaded;
}

struct Session {
  void* Context = nullptr;
  void* Connection = nullptr;
};

static constexpr int kVerifyPeer = 1;
static constexpr int kSetHostName = 55;

extern "C" int64_t wolang_tls_open(int64_t socket, const uint8_t* host, uint64_t length, int32_t verify) {
  const OpenSsl* ssl = LoadOpenSsl();
  if (ssl == nullptr) {
    return 0;
  }

  void* context = ssl->ContextNew(ssl->ClientMethod());
  if (context == nullptr) {
    return 0;
  }
  if (verify != 0) {
    ssl->DefaultPaths(context);
    ssl->SetVerify(context, kVerifyPeer, nullptr);
  }

  void* connection = ssl->New(context);
  if (connection == nullptr) {
    ssl->ContextFree(context);
    return 0;
  }

  std::string name(reinterpret_cast<const char*>(host), static_cast<size_t>(length));
  ssl->Control(connection, kSetHostName, 0, name.data());
  if (verify != 0) {
    ssl->SetHost(connection, name.c_str());
  }
  if (ssl->SetFd(connection, static_cast<int>(socket)) != 1 || ssl->Connect(connection) != 1 || (verify != 0 && ssl->VerifyResult(connection) != 0)) {
    ssl->Free(connection);
    ssl->ContextFree(context);
    return 0;
  }

  auto* session = new Session();
  session->Context = context;
  session->Connection = connection;
  return reinterpret_cast<int64_t>(session);
}

extern "C" int64_t wolang_tls_write(int64_t handle, const uint8_t* bytes, int64_t count) {
  const OpenSsl* ssl = LoadOpenSsl();
  auto* session = reinterpret_cast<Session*>(handle);
  if (ssl == nullptr || session == nullptr) {
    return -1;
  }

  int64_t done = 0;
  while (done < count) {
    const int written = ssl->Write(session->Connection, bytes + done, static_cast<int>(count - done));
    if (written <= 0) {
      return -1;
    }
    done += written;
  }
  return done;
}

extern "C" int64_t wolang_tls_read(int64_t handle, uint8_t* bytes, int64_t room) {
  const OpenSsl* ssl = LoadOpenSsl();
  auto* session = reinterpret_cast<Session*>(handle);
  if (ssl == nullptr || session == nullptr || room <= 0) {
    return -1;
  }

  const int got = ssl->Read(session->Connection, bytes, static_cast<int>(room));
  return got < 0 ? 0 : got;
}

extern "C" void wolang_tls_close(int64_t handle) {
  const OpenSsl* ssl = LoadOpenSsl();
  auto* session = reinterpret_cast<Session*>(handle);
  if (ssl == nullptr || session == nullptr) {
    return;
  }

  ssl->Shutdown(session->Connection);
  ssl->Free(session->Connection);
  ssl->ContextFree(session->Context);
  delete session;
}

#endif

}  // namespace runtime
}  // namespace wolang
