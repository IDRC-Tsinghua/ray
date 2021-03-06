#include "ray/gcs/redis_context.h"

#include <unistd.h>

extern "C" {
#include "hiredis/async.h"
#include "hiredis/hiredis.h"
#include "hiredis/adapters/ae.h"
}

// TODO(pcm): Integrate into the C++ tree.
#include "state/ray_config.h"

namespace ray {

namespace gcs {

// This is a global redis callback which will be registered for every
// asynchronous redis call. It dispatches the appropriate callback
// that was registered with the RedisCallbackManager.
void GlobalRedisCallback(void *c, void *r, void *privdata) {
  if (r == NULL) {
    return;
  }
  int64_t callback_index = reinterpret_cast<int64_t>(privdata);
  redisReply *reply = reinterpret_cast<redisReply *>(r);
  std::string data = "";
  if (reply->type == REDIS_REPLY_NIL) {
  } else if (reply->type == REDIS_REPLY_STRING) {
    data = std::string(reply->str, reply->len);
  } else if (reply->type == REDIS_REPLY_ARRAY) {
    reply = reply->element[reply->elements - 1];
    data = std::string(reply->str, reply->len);
  } else if (reply->type == REDIS_REPLY_STATUS) {
  } else if (reply->type == REDIS_REPLY_ERROR) {
    RAY_LOG(ERROR) << "Redis error " << reply->str;
  } else {
    RAY_LOG(FATAL) << "Fatal redis error of type " << reply->type
                   << " and with string " << reply->str;
  }
  RedisCallbackManager::instance().get(callback_index)(data);
  // Delete the callback.
  RedisCallbackManager::instance().remove(callback_index);
}

void SubscribeRedisCallback(void *c, void *r, void *privdata) {
  if (r == NULL) {
    return;
  }
  int64_t callback_index = reinterpret_cast<int64_t>(privdata);
  redisReply *reply = reinterpret_cast<redisReply *>(r);
  std::string data = "";
  if (reply->type == REDIS_REPLY_ARRAY) {
    // Parse the message.
    redisReply *message_type = reply->element[0];
    if (strcmp(message_type->str, "subscribe") == 0) {
      // If the message is for the initial subscription call, do not fill in
      // data.
    } else if (strcmp(message_type->str, "message") == 0) {
      // If the message is from a PUBLISH, make sure the data is nonempty.
      redisReply *message = reply->element[reply->elements - 1];
      data = std::string(message->str, message->len);
      RAY_CHECK(!data.empty()) << "Empty message received on subscribe channel";
    } else {
      RAY_LOG(FATAL) << "Fatal redis error during subscribe"
                     << message_type->str;
    }

    // NOTE(swang): We do not delete the callback after calling it since there
    // may be more subscription messages.
    RedisCallbackManager::instance().get(callback_index)(data);
  } else if (reply->type == REDIS_REPLY_ERROR) {
    RAY_LOG(ERROR) << "Redis error " << reply->str;
  } else {
    RAY_LOG(FATAL) << "Fatal redis error of type " << reply->type
                   << " and with string " << reply->str;
  }
}

int64_t RedisCallbackManager::add(const RedisCallback &function) {
  callbacks_.emplace(num_callbacks, std::unique_ptr<RedisCallback>(
                                        new RedisCallback(function)));
  return num_callbacks++;
}

RedisCallbackManager::RedisCallback &RedisCallbackManager::get(
    int64_t callback_index) {
  return *callbacks_[callback_index];
}

void RedisCallbackManager::remove(int64_t callback_index) {
  callbacks_.erase(callback_index);
}

#define REDIS_CHECK_ERROR(CONTEXT, REPLY)                     \
  if (REPLY == nullptr || REPLY->type == REDIS_REPLY_ERROR) { \
    return Status::RedisError(CONTEXT->errstr);               \
  }

RedisContext::~RedisContext() {
  if (context_) {
    redisFree(context_);
  }
  if (async_context_) {
    redisAsyncFree(async_context_);
  }
  if (subscribe_context_) {
    redisAsyncFree(subscribe_context_);
  }
}

Status RedisContext::Connect(const std::string &address, int port) {
  int connection_attempts = 0;
  context_ = redisConnect(address.c_str(), port);
  while (context_ == nullptr || context_->err) {
    if (connection_attempts >=
        RayConfig::instance().redis_db_connect_retries()) {
      if (context_ == nullptr) {
        RAY_LOG(FATAL) << "Could not allocate redis context.";
      }
      if (context_->err) {
        RAY_LOG(FATAL) << "Could not establish connection to redis " << address
                       << ":" << port;
      }
      break;
    }
    RAY_LOG(WARNING) << "Failed to connect to Redis, retrying.";
    // Sleep for a little.
    usleep(RayConfig::instance().redis_db_connect_wait_milliseconds() * 1000);
    context_ = redisConnect(address.c_str(), port);
    connection_attempts += 1;
  }
  redisReply *reply = reinterpret_cast<redisReply *>(
      redisCommand(context_, "CONFIG SET notify-keyspace-events Kl"));
  REDIS_CHECK_ERROR(context_, reply);
  freeReplyObject(reply);

  // Connect to async context
  async_context_ = redisAsyncConnect(address.c_str(), port);
  if (async_context_ == nullptr || async_context_->err) {
    RAY_LOG(FATAL) << "Could not establish connection to redis " << address
                   << ":" << port;
  }
  // Connect to subscribe context
  subscribe_context_ = redisAsyncConnect(address.c_str(), port);
  if (subscribe_context_ == nullptr || subscribe_context_->err) {
    RAY_LOG(FATAL) << "Could not establish subscribe connection to redis "
                   << address << ":" << port;
  }
  return Status::OK();
}

Status RedisContext::AttachToEventLoop(aeEventLoop *loop) {
  if (redisAeAttach(loop, async_context_) != REDIS_OK ||
      redisAeAttach(loop, subscribe_context_) != REDIS_OK) {
    return Status::RedisError("could not attach redis event loop");
  } else {
    return Status::OK();
  }
}

Status RedisContext::RunAsync(const std::string &command,
                              const UniqueID &id,
                              uint8_t *data,
                              int64_t length,
                              const TablePubsub pubsub_channel,
                              int64_t callback_index) {
  if (length > 0) {
    std::string redis_command = command + " %d %b %b";
    int status = redisAsyncCommand(
        async_context_,
        reinterpret_cast<redisCallbackFn *>(&GlobalRedisCallback),
        reinterpret_cast<void *>(callback_index), redis_command.c_str(),
        pubsub_channel, id.data(), id.size(), data, length);
    if (status == REDIS_ERR) {
      return Status::RedisError(std::string(async_context_->errstr));
    }
  } else {
    std::string redis_command = command + " %d %b";
    int status = redisAsyncCommand(
        async_context_,
        reinterpret_cast<redisCallbackFn *>(&GlobalRedisCallback),
        reinterpret_cast<void *>(callback_index), redis_command.c_str(),
        pubsub_channel, id.data(), id.size());
    if (status == REDIS_ERR) {
      return Status::RedisError(std::string(async_context_->errstr));
    }
  }
  return Status::OK();
}

Status RedisContext::SubscribeAsync(const ClientID &client_id,
                                    const TablePubsub pubsub_channel,
                                    int64_t callback_index) {
  RAY_CHECK(pubsub_channel != TablePubsub_NO_PUBLISH)
      << "Client requested subscribe on a table that does not support pubsub";

  int status = 0;
  if (client_id.is_nil()) {
    // Subscribe to all messages.
    std::string redis_command = "SUBSCRIBE %d";
    status = redisAsyncCommand(
        subscribe_context_,
        reinterpret_cast<redisCallbackFn *>(&SubscribeRedisCallback),
        reinterpret_cast<void *>(callback_index), redis_command.c_str(),
        pubsub_channel);
  } else {
    // Subscribe only to messages sent to this client.
    // TODO(swang): Nobody sends on this channel yet.
    std::string redis_command = "SUBSCRIBE %d:%b";
    status = redisAsyncCommand(
        subscribe_context_,
        reinterpret_cast<redisCallbackFn *>(&SubscribeRedisCallback),
        reinterpret_cast<void *>(callback_index), redis_command.c_str(),
        pubsub_channel, client_id.data(), client_id.size());
  }

  if (status == REDIS_ERR) {
    return Status::RedisError(std::string(subscribe_context_->errstr));
  }
  return Status::OK();
}

}  // namespace gcs

}  // namespace ray
