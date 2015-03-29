/**
  ******************************************************************************
  * @file    spark_protocol.cpp
  * @authors  Zachary Crockett, Matthew McGowan
  * @version V1.0.0
  * @date    15-Nov-2013
  * @brief   SPARK PROTOCOL
  ******************************************************************************
  Copyright (c) 2013 Spark Labs, Inc.  All rights reserved.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation, either
  version 3 of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this program; if not, see <http://www.gnu.org/licenses/>.
  ******************************************************************************
  */
#include "spark_protocol.h"
#include "handshake.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "device_keys.h"

#ifndef PRODUCT_ID
#define PRODUCT_ID (0xffff)
#endif

#ifndef PRODUCT_FIRMWARE_VERSION
#define PRODUCT_FIRMWARE_VERSION (0xffff)
#endif

/**
 * Handle the cryptographically secure random seed from the cloud by using
 * it to seed the stdlib PRNG. 
 * @param seed  A random value from a cryptographically secure random number generator.
 */
inline void default_random_seed_from_cloud(unsigned int seed)
{
    srand(seed);
}
bool SparkProtocol::is_initialized(void)
{
  return initialized;
}

void SparkProtocol::reset_updating(void)
{
  updating = false;
}

SparkProtocol::SparkProtocol() : QUEUE_SIZE(640), handlers({NULL}), expecting_ping_ack(false),
                                     initialized(false), updating(false), product_id(PRODUCT_ID), product_firmware_version(PRODUCT_FIRMWARE_VERSION)
{
    queue_init();
}


void SparkProtocol::init(const char *id,
                         const SparkKeys &keys,
                         const SparkCallbacks &callbacks,
                         const SparkDescriptor &descriptor)
{
  memcpy(server_public_key, keys.server_public, MAX_SERVER_PUBLIC_KEY_LENGTH);
  memcpy(core_private_key, keys.core_private, MAX_DEVICE_PRIVATE_KEY_LENGTH);
  memcpy(device_id, id, 12);

  // when using this lib in C, constructor is never called
  queue_init();

  callback_send = callbacks.send;
  callback_receive = callbacks.receive;
  callback_prepare_to_save_file = callbacks.prepare_to_save_file;
  callback_prepare_for_firmware_update = callbacks.prepare_for_firmware_update;
  callback_finish_firmware_update = callbacks.finish_firmware_update;
  callback_calculate_crc = callbacks.calculate_crc;
  callback_save_firmware_chunk = callbacks.save_firmware_chunk;
  callback_signal = callbacks.signal;
  callback_millis = callbacks.millis;
  callback_set_time = callbacks.set_time;

  this->descriptor.num_functions = descriptor.num_functions;
  this->descriptor.copy_function_key = descriptor.copy_function_key;
  this->descriptor.call_function = descriptor.call_function;
  this->descriptor.num_variables = descriptor.num_variables;
  this->descriptor.copy_variable_key = descriptor.copy_variable_key;
  this->descriptor.variable_type = descriptor.variable_type;
  this->descriptor.get_variable = descriptor.get_variable;
  this->descriptor.was_ota_upgrade_successful = descriptor.was_ota_upgrade_successful;
  this->descriptor.ota_upgrade_status_sent = descriptor.ota_upgrade_status_sent;

  memset(event_handlers, 0, sizeof(event_handlers));

  initialized = true;
}

int SparkProtocol::handshake(void)
{
  memcpy(queue + 40, device_id, 12);
  int err = blocking_receive(queue, 40);
  if (0 > err) return err;

  parse_device_pubkey_from_privkey(queue+52, core_private_key);
  
  rsa_context rsa;
  init_rsa_context_with_public_key(&rsa, server_public_key);
  const int len = 52+MAX_DEVICE_PUBLIC_KEY_LENGTH;
  err = rsa_pkcs1_encrypt(&rsa, RSA_PUBLIC, len, queue, queue + len);
  rsa_free(&rsa);

  if (err) return err;

  blocking_send(queue + len, 256);
  err = blocking_receive(queue, 384);
  if (0 > err) return err;

  err = set_key(queue);
  if (err) return err;

  queue[0] = 0x00;
  queue[1] = 0x10;
  hello(queue + 2, descriptor.was_ota_upgrade_successful());

  blocking_send(queue, 18);

  return 0;
}

// Returns true if no errors and still connected.
// Returns false if there was an error, and we are probably disconnected.
bool SparkProtocol::event_loop(void)
{
  int bytes_received = callback_receive(queue, 2);
  if (2 <= bytes_received)
  {
    bool success = handle_received_message();
    if (!success)
    {
      // bail if and only if there was an error
      return false;
    }
  }
  else
  {
    if (0 > bytes_received)
    {
      // error, disconnected
      return false;
    }

    if (updating)
    {
      system_tick_t millis_since_last_chunk = callback_millis() - last_chunk_millis;

      if (3000 < millis_since_last_chunk)
      {
        queue[0] = 0;
        queue[1] = 16;
        chunk_missed(queue + 2, chunk_index);
        if (0 > blocking_send(queue, 18))
        {
          // error
          return false;
        }

        last_chunk_millis = callback_millis();
      }
    }
    else
    {
      system_tick_t millis_since_last_message = callback_millis() - last_message_millis;
      if (expecting_ping_ack)
      {
        if (10000 < millis_since_last_message)
        {
          // timed out, disconnect
          expecting_ping_ack = false;
          last_message_millis = callback_millis();
          return false;
        }
      }
      else
      {
        if (15000 < millis_since_last_message)
        {
          queue[0] = 0;
          queue[1] = 16;
          ping(queue + 2);
          blocking_send(queue, 18);

          expecting_ping_ack = true;
          last_message_millis = callback_millis();
        }
      }
    }
  }

  // no errors, still connected
  return true;
}

// Returns bytes sent or -1 on error
int SparkProtocol::blocking_send(const unsigned char *buf, int length)
{
  int bytes_or_error;
  int byte_count = 0;

  system_tick_t _millis = callback_millis();

  while (length > byte_count)
  {
    bytes_or_error = callback_send(buf + byte_count, length - byte_count);
    if (0 > bytes_or_error)
    {
      // error, disconnected
      return bytes_or_error;
    }
    else if (0 < bytes_or_error)
    {
      byte_count += bytes_or_error;
    }
    else
    {
      if (20000 < (callback_millis() - _millis))
      {
        // timed out, disconnect
        return -1;
      }
    }
  }
  return byte_count;
}

// Returns bytes received or -1 on error
int SparkProtocol::blocking_receive(unsigned char *buf, int length)
{
  int bytes_or_error;
  int byte_count = 0;

  system_tick_t _millis = callback_millis();

  while (length > byte_count)
  {
    bytes_or_error = callback_receive(buf + byte_count, length - byte_count);
    if (0 > bytes_or_error)
    {
      // error, disconnected
      return bytes_or_error;
    }
    else if (0 < bytes_or_error)
    {
      byte_count += bytes_or_error;
    }
    else
    {
      if (20000 < (callback_millis() - _millis))
      {
        // timed out, disconnect
        return -1;
      }
    }
  }
  return byte_count;
}

CoAPMessageType::Enum
  SparkProtocol::received_message(unsigned char *buf, int length)
{
  unsigned char next_iv[16];
  memcpy(next_iv, buf, 16);

  aes_setkey_dec(&aes, key, 128);
  aes_crypt_cbc(&aes, AES_DECRYPT, length, iv_receive, buf, buf);

  memcpy(iv_receive, next_iv, 16);

  char path = buf[ 5 + (buf[0] & 0x0F) ];

  switch (CoAP::code(buf))
  {
    case CoAPCode::GET:
      switch (path)
      {
        case 'v': return CoAPMessageType::VARIABLE_REQUEST;
        case 'd': return CoAPMessageType::DESCRIBE;
        default: break;
      } break;
    case CoAPCode::POST:
      switch (path)
      {
        case 'E':
        case 'e':
          return CoAPMessageType::EVENT;
        case 'h': return CoAPMessageType::HELLO;
        case 'f': return CoAPMessageType::FUNCTION_CALL;
        case 's': return CoAPMessageType::SAVE_BEGIN;
        case 'u': return CoAPMessageType::UPDATE_BEGIN;
        case 'c': return CoAPMessageType::CHUNK;
        default: break;
      } break;
    case CoAPCode::PUT:
      switch (path)
      {
        case 'k': return CoAPMessageType::KEY_CHANGE;
        case 'u': return CoAPMessageType::UPDATE_DONE;
        case 's':
          if (buf[8]) return CoAPMessageType::SIGNAL_START;
          else return CoAPMessageType::SIGNAL_STOP;
        default: break;
      } break;
    case CoAPCode::EMPTY:
      switch (CoAP::type(buf))
      {
        case CoAPType::CON: return CoAPMessageType::PING;
        default: return CoAPMessageType::EMPTY_ACK;
      } break;
    case CoAPCode::CONTENT:
      return CoAPMessageType::TIME;
    default:
      break;
  }
  return CoAPMessageType::ERROR;
}

void SparkProtocol::hello(unsigned char *buf, bool newly_upgraded)
{
  unsigned short message_id = next_message_id();

  buf[0] = 0x50; // non-confirmable, no token
  buf[1] = 0x02; // POST
  buf[2] = message_id >> 8;
  buf[3] = message_id & 0xff;
  buf[4] = 0xb1; // Uri-Path option of length 1
  buf[5] = 'h';
  buf[6] = 0xff; // payload marker
  buf[7] = product_id >> 8;
  buf[8] = product_id & 0xff;
  buf[9] = product_firmware_version >> 8;
  buf[10] = product_firmware_version & 0xff;
  buf[11] = 0; // reserved flags
  buf[12] = newly_upgraded ? 1 : 0;

  memset(buf + 13, 3, 3); // PKCS #7 padding

  encrypt(buf, 16);
}

void SparkProtocol::key_changed(unsigned char *buf, unsigned char token)
{
  separate_response(buf, token, 0x44);
}

void SparkProtocol::function_return(unsigned char *buf,
                                    unsigned char token,
                                    int return_value)
{
  unsigned short message_id = next_message_id();

  buf[0] = 0x51; // non-confirmable, one-byte token
  buf[1] = 0x44; // response code 2.04 CHANGED
  buf[2] = message_id >> 8;
  buf[3] = message_id & 0xff;
  buf[4] = token;
  buf[5] = 0xff; // payload marker
  buf[6] = return_value >> 24;
  buf[7] = return_value >> 16 & 0xff;
  buf[8] = return_value >> 8 & 0xff;
  buf[9] = return_value & 0xff;

  memset(buf + 10, 6, 6); // PKCS #7 padding

  encrypt(buf, 16);
}

void SparkProtocol::variable_value(unsigned char *buf,
                                   unsigned char token,
                                   unsigned char message_id_msb,
                                   unsigned char message_id_lsb,
                                   bool return_value)
{
  buf[0] = 0x61; // acknowledgment, one-byte token
  buf[1] = 0x45; // response code 2.05 CONTENT
  buf[2] = message_id_msb;
  buf[3] = message_id_lsb;
  buf[4] = token;
  buf[5] = 0xff; // payload marker
  buf[6] = return_value ? 1 : 0;

  memset(buf + 7, 9, 9); // PKCS #7 padding

  encrypt(buf, 16);
}

void SparkProtocol::variable_value(unsigned char *buf,
                                   unsigned char token,
                                   unsigned char message_id_msb,
                                   unsigned char message_id_lsb,
                                   int return_value)
{
  buf[0] = 0x61; // acknowledgment, one-byte token
  buf[1] = 0x45; // response code 2.05 CONTENT
  buf[2] = message_id_msb;
  buf[3] = message_id_lsb;
  buf[4] = token;
  buf[5] = 0xff; // payload marker
  buf[6] = return_value >> 24;
  buf[7] = return_value >> 16 & 0xff;
  buf[8] = return_value >> 8 & 0xff;
  buf[9] = return_value & 0xff;

  memset(buf + 10, 6, 6); // PKCS #7 padding

  encrypt(buf, 16);
}

void SparkProtocol::variable_value(unsigned char *buf,
                                   unsigned char token,
                                   unsigned char message_id_msb,
                                   unsigned char message_id_lsb,
                                   double return_value)
{
  buf[0] = 0x61; // acknowledgment, one-byte token
  buf[1] = 0x45; // response code 2.05 CONTENT
  buf[2] = message_id_msb;
  buf[3] = message_id_lsb;
  buf[4] = token;
  buf[5] = 0xff; // payload marker

  memcpy(buf + 6, &return_value, 8);

  memset(buf + 14, 2, 2); // PKCS #7 padding

  encrypt(buf, 16);
}

// Returns the length of the buffer to send
int SparkProtocol::variable_value(unsigned char *buf,
                                  unsigned char token,
                                  unsigned char message_id_msb,
                                  unsigned char message_id_lsb,
                                  const void *return_value,
                                  int length)
{
  buf[0] = 0x61; // acknowledgment, one-byte token
  buf[1] = 0x45; // response code 2.05 CONTENT
  buf[2] = message_id_msb;
  buf[3] = message_id_lsb;
  buf[4] = token;
  buf[5] = 0xff; // payload marker

  memcpy(buf + 6, return_value, length);

  int msglen = 6 + length;
  int buflen = (msglen & ~15) + 16;
  char pad = buflen - msglen;
  memset(buf + msglen, pad, pad); // PKCS #7 padding

  encrypt(buf, buflen);

  return buflen;
}

inline bool is_system(const char* event_name) {
    // if there were a strncmpi this would be easier!
    char prefix[6];    
    if (!*event_name || strlen(event_name)<5)
        return false;
    memcpy(prefix, event_name, 5);
    prefix[5] = '\0';
    return !strcasecmp(prefix, "spark");
}

// Returns true on success, false on sending timeout or rate-limiting failure
bool SparkProtocol::send_event(const char *event_name, const char *data,
                               int ttl, EventType::Enum event_type)
{
  if (updating)
  {
    return false;
  }

  bool is_system_event = is_system(event_name);
  
  if (is_system_event) {
      static uint16_t lastMinute = 0;
      static uint8_t eventsThisMinute = 0;
      
      uint16_t currentMinute = uint16_t(callback_millis()>>16);
      if (currentMinute==lastMinute) {      // == handles millis() overflow
          if (eventsThisMinute==255)
              return false;
      }
      else {
          lastMinute = currentMinute;
          eventsThisMinute = 0;
      }
      eventsThisMinute++;      
  }
  else {
    static system_tick_t recent_event_ticks[5] = {
      (system_tick_t) -1000, (system_tick_t) -1000,
      (system_tick_t) -1000, (system_tick_t) -1000,
      (system_tick_t) -1000 };
    static int evt_tick_idx = 0;

    system_tick_t now = recent_event_ticks[evt_tick_idx] = callback_millis();
    evt_tick_idx++;
    evt_tick_idx %= 5;
    if (now - recent_event_ticks[evt_tick_idx] < 1000)
    {
      // exceeded allowable burst of 4 events per second
      return false;
    }
  }
  uint16_t msg_id = next_message_id();
  size_t msglen = event(queue + 2, msg_id, event_name, data, ttl, event_type);
  size_t wrapped_len = wrap(queue, msglen);

  return (0 <= blocking_send(queue, wrapped_len));
}

size_t SparkProtocol::time_request(unsigned char *buf)
{
  unsigned char *p = buf;

  *p++ = 0x41; // Confirmable, one-byte token
  *p++ = 0x01; // GET request

  uint16_t msg_id = next_message_id();
  *p++ = msg_id >> 8;
  *p++ = msg_id & 0xff;

  *p++ = next_token();
  *p++ = 0xb1; // One-byte, Uri-Path option
  *p++ = 't';

  return p - buf;
}

// returns true on success, false on failure
bool SparkProtocol::send_time_request(void)
{
  if (updating)
  {
    return false;
  }

  size_t msglen = time_request(queue + 2);
  size_t wrapped_len = wrap(queue, msglen);

  return (0 <= blocking_send(queue, wrapped_len));
}

bool SparkProtocol::send_subscription(const char *event_name, const char *device_id)
{
  uint16_t msg_id = next_message_id();
  size_t msglen = subscription(queue + 2, msg_id, event_name, device_id);

  size_t buflen = (msglen & ~15) + 16;
  char pad = buflen - msglen;
  memset(queue + 2 + msglen, pad, pad); // PKCS #7 padding

  encrypt(queue + 2, buflen);

  queue[0] = (buflen >> 8) & 0xff;
  queue[1] = buflen & 0xff;

  return (0 <= blocking_send(queue, buflen + 2));
}

bool SparkProtocol::send_subscription(const char *event_name,
                                      SubscriptionScope::Enum scope)
{
  uint16_t msg_id = next_message_id();
  size_t msglen = subscription(queue + 2, msg_id, event_name, scope);

  size_t buflen = (msglen & ~15) + 16;
  char pad = buflen - msglen;
  memset(queue + 2 + msglen, pad, pad); // PKCS #7 padding

  encrypt(queue + 2, buflen);

  queue[0] = (buflen >> 8) & 0xff;
  queue[1] = buflen & 0xff;

  return (0 <= blocking_send(queue, buflen + 2));
}

void SparkProtocol::send_subscriptions()
{
  const int NUM_HANDLERS = sizeof(event_handlers) / sizeof(FilteringEventHandler);
  for (int i = 0; i < NUM_HANDLERS; i++)
  {
    if (NULL != event_handlers[i].handler)
    {
        if (event_handlers[i].device_id[0]) 
        {
            send_subscription(event_handlers[i].filter, event_handlers[i].device_id);
        }
        else
        {
            send_subscription(event_handlers[i].filter, event_handlers[i].scope);
        }
    }
  }
}

void SparkProtocol::remove_event_handlers(const char* event_name) 
{
    if (NULL == event_name)
    {
        memset(event_handlers, 0, sizeof(event_handlers));
    }
    else
    {        
        const int NUM_HANDLERS = sizeof(event_handlers) / sizeof(FilteringEventHandler);
        int dest = 0;
        for (int i = 0; i < NUM_HANDLERS; i++)
        {
          if (NULL != event_handlers[i].filter && !strcmp(event_name, event_handlers[i].filter))
          {
              memset(&event_handlers[i], 0, sizeof(event_handlers[i]));
          }
          else
          {
              if (dest!=i) {
                memcpy(event_handlers+dest, event_handlers+i, sizeof(event_handlers[i]));
                memset(event_handlers+i, 0, sizeof(event_handlers[i]));
              }
              dest++;
          }
        }
    }
}

bool SparkProtocol::event_handler_exists(const char *event_name, EventHandler handler, 
    SubscriptionScope::Enum scope, const char* id)
{
  const int NUM_HANDLERS = sizeof(event_handlers) / sizeof(FilteringEventHandler);
  for (int i = 0; i < NUM_HANDLERS; i++)
  {
      if (event_handlers[i].handler==handler && event_handlers[i].scope==scope) {
        const size_t MAX_FILTER_LEN = sizeof(event_handlers[i].filter);
        const size_t FILTER_LEN = strnlen(event_name, MAX_FILTER_LEN);
        if (!strncmp(event_handlers[i].filter, event_name, FILTER_LEN)) {
            const size_t MAX_ID_LEN = sizeof(event_handlers[i].device_id)-1;
            const size_t id_len = id ? strnlen(id, MAX_ID_LEN) : 0;
            if (id_len)
                return !strncmp(event_handlers[i].device_id, id, id_len);
            else
                return !event_handlers[i].device_id[0];
        }
      }
  }   
  return false;
}

bool SparkProtocol::add_event_handler(const char *event_name, EventHandler handler, 
    SubscriptionScope::Enum scope, const char* id)
{
    if (event_handler_exists(event_name, handler, scope, id))
        return true;
    
  const int NUM_HANDLERS = sizeof(event_handlers) / sizeof(FilteringEventHandler);
  for (int i = 0; i < NUM_HANDLERS; i++)
  {
    if (NULL == event_handlers[i].handler)
    {
      const size_t MAX_FILTER_LEN = sizeof(event_handlers[i].filter);
      const size_t FILTER_LEN = strnlen(event_name, MAX_FILTER_LEN);
      memcpy(event_handlers[i].filter, event_name, FILTER_LEN);
      memset(event_handlers[i].filter + FILTER_LEN, 0, MAX_FILTER_LEN - FILTER_LEN);
      event_handlers[i].handler = handler;
      event_handlers[i].device_id[0] = 0;
        const size_t MAX_ID_LEN = sizeof(event_handlers[i].device_id)-1;
        const size_t id_len = id ? strnlen(id, MAX_ID_LEN) : 0;
        memcpy(event_handlers[i].device_id, id, id_len);
        event_handlers[i].device_id[id_len] = 0;
        event_handlers[i].scope = scope;
      return true;
    }
  }
  return false;
}

void SparkProtocol::chunk_received(unsigned char *buf,
                                   unsigned char token,
                                   ChunkReceivedCode::Enum code)
{
  separate_response(buf, token, code);
}

void SparkProtocol::chunk_missed(unsigned char *buf, unsigned short chunk_index)
{
  unsigned short message_id = next_message_id();

  buf[0] = 0x40; // confirmable, no token
  buf[1] = 0x01; // code 0.01 GET
  buf[2] = message_id >> 8;
  buf[3] = message_id & 0xff;
  buf[4] = 0xb1; // one-byte Uri-Path option
  buf[5] = 'c';
  buf[6] = 0xff; // payload marker
  buf[7] = chunk_index >> 8;
  buf[8] = chunk_index & 0xff;

  memset(buf + 9, 7, 7); // PKCS #7 padding

  encrypt(buf, 16);
}

void SparkProtocol::update_ready(unsigned char *buf, unsigned char token)
{
    unsigned char payload[1];
    payload[0] = 1;
    separate_response_with_payload(buf, token, 0x44, payload, sizeof(payload));
}

int SparkProtocol::description(unsigned char *buf, unsigned char token,
                               unsigned char message_id_msb, unsigned char message_id_lsb)
{
  buf[0] = 0x61; // acknowledgment, one-byte token
  buf[1] = 0x45; // response code 2.05 CONTENT
  buf[2] = message_id_msb;
  buf[3] = message_id_lsb;
  buf[4] = token;
  buf[5] = 0xff; // payload marker

  memcpy(buf + 6, "{\"f\":[", 6);

  char *buf_ptr = (char *)buf + 12;

  int num_keys = descriptor.num_functions();
  int i;
  for (i = 0; i < num_keys; ++i)
  {
    if (i)
    {
      *buf_ptr = ',';
      ++buf_ptr;
    }
    *buf_ptr = '"';
    ++buf_ptr;
    descriptor.copy_function_key(buf_ptr, i);
    int function_name_length = strlen(buf_ptr);
    if (MAX_FUNCTION_KEY_LENGTH < function_name_length)
    {
      function_name_length = MAX_FUNCTION_KEY_LENGTH;
    }
    buf_ptr += function_name_length;
    *buf_ptr = '"';
    ++buf_ptr;
  }

  memcpy(buf_ptr, "],\"v\":{", 7);
  buf_ptr += 7;

  num_keys = descriptor.num_variables();
  for (i = 0; i < num_keys; ++i)
  {
    if (i)
    {
      *buf_ptr = ',';
      ++buf_ptr;
    }
    *buf_ptr = '"';
    ++buf_ptr;
    descriptor.copy_variable_key(buf_ptr, i);
    int variable_name_length = strlen(buf_ptr);
    SparkReturnType::Enum t = descriptor.variable_type(buf_ptr);
    if (MAX_VARIABLE_KEY_LENGTH < variable_name_length)
    {
      variable_name_length = MAX_VARIABLE_KEY_LENGTH;
    }
    buf_ptr += variable_name_length;
    memcpy(buf_ptr, "\":", 2);
    buf_ptr += 2;
    *buf_ptr = '0' + (char)t;
    ++buf_ptr;
  }

  memcpy(buf_ptr, "}}", 2);
  buf_ptr += 2;

  int msglen = buf_ptr - (char *)buf;
  int buflen = (msglen & ~15) + 16;
  char pad = buflen - msglen;
  memset(buf_ptr, pad, pad); // PKCS #7 padding

  encrypt(buf, buflen);
  return buflen;
}

void SparkProtocol::ping(unsigned char *buf)
{
  unsigned short message_id = next_message_id();

  buf[0] = 0x40; // Confirmable, no token
  buf[1] = 0x00; // code signifying empty message
  buf[2] = message_id >> 8;
  buf[3] = message_id & 0xff;

  memset(buf + 4, 12, 12); // PKCS #7 padding

  encrypt(buf, 16);
}

int SparkProtocol::presence_announcement(unsigned char *buf, const char *id)
{
  buf[0] = 0x50; // Confirmable, no token
  buf[1] = 0x02; // Code POST
  buf[2] = 0x00; // message id ignorable in this context
  buf[3] = 0x00;
  buf[4] = 0xb1; // Uri-Path option of length 1
  buf[5] = 'h';
  buf[6] = 0xff; // payload marker

  memcpy(buf + 7, id, 12);

  return 19;
}


/********** Queue **********/

int SparkProtocol::queue_bytes_available()
{
  int unoccupied = queue_front - queue_back - 1;
  if (unoccupied < 0)
    return unoccupied + QUEUE_SIZE;
  else
    return unoccupied;
}

int SparkProtocol::queue_push(const char *src, int length)
{
  int available = queue_bytes_available();
  if (queue_back >= queue_front)
  {
    int tail_available = queue_mem_boundary - queue_back;
    if (length <= available)
    {
      if (length <= tail_available)
      {
        memcpy(queue_back, src, length);
        queue_back += length;
      }
      else
      {
        int head_needed = length - tail_available;
        memcpy(queue_back, src, tail_available);
        memcpy(queue, src + tail_available, head_needed);
        queue_back = queue + head_needed;
      }
      return length;
    }
    else
    {
      // queue_back is greater than or equal to queue_front
      // and length is greater than available
      if (available < tail_available)
      {
        // queue_front is equal to queue, so don't fill the last bucket
        memcpy(queue_back, src, available);
        queue_back += available;
      }
      else
      {
        int head_available = available - tail_available;
        memcpy(queue_back, src, tail_available);
        memcpy(queue, src + tail_available, head_available);
        queue_back = queue + head_available;
      }
      return available;
    }
  }
  else
  {
    // queue_back is less than queue_front
    int count = length < available ? length : available;
    memcpy(queue_back, src, count);
    queue_back += count;
    return count;
  }
}

int SparkProtocol::queue_pop(char *dst, int length)
{
  if (queue_back >= queue_front)
  {
    int filled = queue_back - queue_front;
    int count = length <= filled ? length : filled;

    memcpy(dst, queue_front, count);
    queue_front += count;
    return count;
  }
  else
  {
    int tail_filled = queue_mem_boundary - queue_front;
    int head_requested = length - tail_filled;
    int head_filled = queue_back - queue;
    int head_count = head_requested < head_filled ? head_requested : head_filled;
    
    memcpy(dst, queue_front, tail_filled);
    memcpy(dst + tail_filled, queue, head_count);
    queue_front = queue + head_count;
    return tail_filled + head_count;
  }
}

ProtocolState::Enum SparkProtocol::state()
{
  return ProtocolState::READ_NONCE;
}


/********** Private methods **********/

size_t SparkProtocol::wrap(unsigned char *buf, size_t msglen)
{
  size_t buflen = (msglen & ~15) + 16;
  char pad = buflen - msglen;
  memset(buf + 2 + msglen, pad, pad); // PKCS #7 padding

  encrypt(buf + 2, buflen);

  buf[0] = (buflen >> 8) & 0xff;
  buf[1] = buflen & 0xff;

  return buflen + 2;
}

bool SparkProtocol::handle_received_message(void)
{
  last_message_millis = callback_millis();
  expecting_ping_ack = false;
  int len = queue[0] << 8 | queue[1];
  if (len > QUEUE_SIZE) { // TODO add sanity check on data, e.g. CRC
      return false;
  }
  if (0 > blocking_receive(queue, len))
  {
    // error
    return false;
  }
  CoAPMessageType::Enum message_type = received_message(queue, len);
  unsigned char token = queue[4];
  unsigned char *msg_to_send = queue + len;
  switch (message_type)
  {
    case CoAPMessageType::DESCRIBE:
    {
      int desc_len = description(queue + 2, token, queue[2], queue[3]);
      queue[0] = (desc_len >> 8) & 0xff;
      queue[1] = desc_len & 0xff;
      if (0 > blocking_send(queue, desc_len + 2))
      {
        // error
        return false;
      }
      break;
    }
    case CoAPMessageType::FUNCTION_CALL:
    {
      // send ACK
      *msg_to_send = 0;
      *(msg_to_send + 1) = 16;
      empty_ack(msg_to_send + 2, queue[2], queue[3]);
      if (0 > blocking_send(msg_to_send, 18))
      {
        // error
        return false;
      }

      // copy the function key
      char function_key[13];
      memset(function_key, 0, 13);
      int function_key_length = queue[7] & 0x0F;
      memcpy(function_key, queue + 8, function_key_length);

      // How long is the argument?
      int q_index = 8 + function_key_length;
      int query_length = queue[q_index] & 0x0F;
      if (13 == query_length)
      {
        ++q_index;
        query_length = 13 + queue[q_index];
      }
      else if (14 == query_length)
      {
        ++q_index;
        query_length = queue[q_index] << 8;
        ++q_index;
        query_length |= queue[q_index];
        query_length += 269;
      }

      // allocated memory bounds check
      if (MAX_FUNCTION_ARG_LENGTH <= query_length)
      {
        return false;
      }

      // save a copy of the argument
      memcpy(function_arg, queue + q_index + 1, query_length);
      function_arg[query_length] = 0; // null terminate string

      // call the given user function
      int return_value = descriptor.call_function(function_key, function_arg);

      // send return value
      *msg_to_send = 0;
      *(msg_to_send + 1) = 16;
      function_return(msg_to_send + 2, token, return_value);
      if (0 > blocking_send(msg_to_send, 18))
      {
        // error
        return false;
      }
      break;
    }
    case CoAPMessageType::VARIABLE_REQUEST:
    {
      // copy the variable key
      int variable_key_length = queue[7] & 0x0F;
      if (12 < variable_key_length)
        variable_key_length = 12;

      char variable_key[13];
      memcpy(variable_key, queue + 8, variable_key_length);
      memset(variable_key + variable_key_length, 0, 13 - variable_key_length);

      queue[0] = 0;
      queue[1] = 16; // default buffer length

      // get variable value according to type using the descriptor
      SparkReturnType::Enum var_type = descriptor.variable_type(variable_key);
      if(SparkReturnType::BOOLEAN == var_type)
      {
        bool *bool_val = (bool *)descriptor.get_variable(variable_key);
        variable_value(queue + 2, token, queue[2], queue[3], *bool_val);
      }
      else if(SparkReturnType::INT == var_type)
      {
        int *int_val = (int *)descriptor.get_variable(variable_key);
        variable_value(queue + 2, token, queue[2], queue[3], *int_val);
      }
      else if(SparkReturnType::STRING == var_type)
      {
        char *str_val = (char *)descriptor.get_variable(variable_key);

        // 2-byte leading length, 16 potential padding bytes
        int max_length = QUEUE_SIZE - 2 - 16;
        int str_length = strlen(str_val);
        if (str_length > max_length) {
          str_length = max_length;
        }

        int buf_size = variable_value(queue + 2, token, queue[2], queue[3], str_val, str_length);
        queue[1] = buf_size & 0xff;
        queue[0] = (buf_size >> 8) & 0xff;
      }
      else if(SparkReturnType::DOUBLE == var_type)
      {
        double *double_val = (double *)descriptor.get_variable(variable_key);
        variable_value(queue + 2, token, queue[2], queue[3], *double_val);
      }

      // buffer length may have changed if variable is a long string
      if (0 > blocking_send(queue, (queue[0] << 8) + queue[1] + 2))
      {
        // error
        return false;
      }
      break;
    }
    case CoAPMessageType::CHUNK:
    {
      last_chunk_millis = callback_millis();

      // send ACK
      *msg_to_send = 0;
      *(msg_to_send + 1) = 16;
      empty_ack(msg_to_send + 2, queue[2], queue[3]);
      if (0 > blocking_send(msg_to_send, 18))
      {
        // error
        return false;
      }

      // check crc
      unsigned int given_crc = queue[8] << 24 | queue[9] << 16 | queue[10] << 8 | queue[11];
      if (callback_calculate_crc(queue + 13, len - 13 - queue[len - 1]) == given_crc || true)
      {
        unsigned short next_chunk_index = callback_save_firmware_chunk(queue + 15, len - 15 - queue[len - 1]);
        if (next_chunk_index > chunk_index)
        {
          chunk_received(msg_to_send + 2, token, ChunkReceivedCode::OK);
        }
        else
        {
          chunk_missed(msg_to_send + 2, next_chunk_index);
        }
        chunk_index = next_chunk_index;
      }
      else
      {
        chunk_received(msg_to_send + 2, token, ChunkReceivedCode::BAD);
      }
      if (0 > blocking_send(msg_to_send, 18))
      {
        // error
        return false;
      }
      break;
    }
    case CoAPMessageType::SAVE_BEGIN:
      // fall through
    case CoAPMessageType::UPDATE_BEGIN:
      // send ACK
      *msg_to_send = 0;
      *(msg_to_send + 1) = 16;
      empty_ack(msg_to_send + 2, queue[2], queue[3]);
      if (0 > blocking_send(msg_to_send, 18))
      {
        // error
        return false;
      }

      if(message_type == CoAPMessageType::SAVE_BEGIN)
      {
        // save file to the specified location in external flash
        unsigned int sflash_address = queue[8] << 24 | queue[9] << 16 | queue[10] << 8 | queue[11];
        // file size information is required to calculate the sectors that need to be erased
        unsigned int file_size = queue[12] << 24 | queue[13] << 16 | queue[14] << 8 | queue[15];
        callback_prepare_to_save_file(sflash_address, file_size);
      }
      else
      {
        callback_prepare_for_firmware_update();
      }

      last_chunk_millis = callback_millis();
      chunk_index = 0;
      updating = true;

      // send update_reaady
      update_ready(msg_to_send + 2, token);
      if (0 > blocking_send(msg_to_send, 18))
      {
        // error
        return false;
      }
      break;
    case CoAPMessageType::UPDATE_DONE:
      // send ACK 2.04
      *msg_to_send = 0;
      *(msg_to_send + 1) = 16;
      coded_ack(msg_to_send + 2, token, ChunkReceivedCode::OK, queue[2], queue[3]);
      if (0 > blocking_send(msg_to_send, 18))
      {
        // error
        return false;
      }

      updating = false;
      callback_finish_firmware_update(true);
      break;
    case CoAPMessageType::EVENT:
    { 
        // fist decode the event data before looking for a handler
        unsigned char pad = queue[len - 1];
        if (0 == pad || 16 < pad)
        {
          // ignore bad message, PKCS #7 padding must be 1-16
          break;
        }
        // end of CoAP message
        unsigned char *end = queue + len - pad;

        unsigned char *event_name = queue + 6;
        size_t event_name_length = CoAP::option_decode(&event_name);
        if (0 == event_name_length)
        {
          // error, malformed CoAP option
          break;
        }

        unsigned char *next_src = event_name + event_name_length;
        unsigned char *next_dst = next_src;
        while (next_src < end && 0x00 == (*next_src & 0xf0))
        {
          // there's another Uri-Path option, i.e., event name with slashes
          size_t option_len = CoAP::option_decode(&next_src);
          *next_dst++ = '/';
          if (next_dst != next_src)
          {
            // at least one extra byte has been used to encode a CoAP Uri-Path option length
            memmove(next_dst, next_src, option_len);
          }
          next_src += option_len;
          next_dst += option_len;
        }
        event_name_length = next_dst - event_name;

        if (next_src < end && 0x30 == (*next_src & 0xf0))
        {
          // Max-Age option is next, which we ignore
          size_t next_len = CoAP::option_decode(&next_src);
          next_src += next_len;
        }

        unsigned char *data = NULL;
        if (next_src < end && 0xff == *next_src)
        {
          // payload is next
          data = next_src + 1;
          // null terminate data string
          *end = 0;
        }
        // null terminate event name string
        event_name[event_name_length] = 0;

      const int NUM_HANDLERS = sizeof(event_handlers) / sizeof(EventHandler);
      for (int i = 0; i < NUM_HANDLERS; i++)
      {
        if (NULL == event_handlers[i].handler)
        {
           break;
        }
        const size_t MAX_FILTER_LENGTH = sizeof(event_handlers[i].filter);
        const size_t filter_length = strnlen(event_handlers[i].filter, MAX_FILTER_LENGTH);
        
        if (event_name_length < filter_length)
        {
          // does not match this filter, try the next event handler
          continue;
        }

        const int cmp = memcmp(event_handlers[i].filter, event_name, filter_length);
        if (0 == cmp)
        {        
            event_handlers[i].handler((char *)event_name, (char *)data);                    
            break;
        }
        // else continue the for loop to try the next handler
      }
      break;
    }
    case CoAPMessageType::KEY_CHANGE:
      // TODO
      break;
    case CoAPMessageType::SIGNAL_START:
      queue[0] = 0;
      queue[1] = 16;
      coded_ack(queue + 2, token, ChunkReceivedCode::OK, queue[2], queue[3]);
      if (0 > blocking_send(queue, 18))
      {
        // error
        return false;
      }

      callback_signal(true);
      break;
    case CoAPMessageType::SIGNAL_STOP:
      queue[0] = 0;
      queue[1] = 16;
      coded_ack(queue + 2, token, ChunkReceivedCode::OK, queue[2], queue[3]);
      if (0 > blocking_send(queue, 18))
      {
        // error
        return false;
      }

      callback_signal(false);
      break;

    case CoAPMessageType::HELLO:
      descriptor.ota_upgrade_status_sent();
      break;

    case CoAPMessageType::TIME:
      callback_set_time(queue[6] << 24 | queue[7] << 16 | queue[8] << 8 | queue[9]);
      break;

    case CoAPMessageType::PING:
      *msg_to_send = 0;
      *(msg_to_send + 1) = 16;
      empty_ack(msg_to_send + 2, queue[2], queue[3]);
      if (0 > blocking_send(msg_to_send, 18))
      {
        // error
        return false;
      }
      break;

    case CoAPMessageType::EMPTY_ACK:
    case CoAPMessageType::ERROR:
    default:
      ; // drop it on the floor
  }

  // all's well
  return true;
}

unsigned short SparkProtocol::next_message_id()
{
  return ++_message_id;
}

unsigned char SparkProtocol::next_token()
{
  return ++_token;
}

void SparkProtocol::encrypt(unsigned char *buf, int length)
{
  aes_setkey_enc(&aes, key, 128);
  aes_crypt_cbc(&aes, AES_ENCRYPT, length, iv_send, buf, buf);
  memcpy(iv_send, buf, 16);
}

void SparkProtocol::separate_response(unsigned char *buf,
                                      unsigned char token,
                                      unsigned char code)
{
    separate_response_with_payload(buf, token, code, NULL, 0);
}

void SparkProtocol::separate_response_with_payload(unsigned char *buf,
                                      unsigned char token,
                                      unsigned char code,
                                      unsigned char* payload,
                                      unsigned payload_len)
{
  unsigned short message_id = next_message_id();

  buf[0] = 0x51; // non-confirmable, one-byte token
  buf[1] = code;
  buf[2] = message_id >> 8;
  buf[3] = message_id & 0xff;
  buf[4] = token;
  
  unsigned len = 5;
  // for now, assume the payload is less than 9
  if (payload && payload_len) {
      buf[5] = 0xFF;
      memcpy(buf+6, payload, payload_len);
      len += 1 + payload_len;
  }
  
  memset(buf + len, 16-len, 16-len); // PKCS #7 padding

  encrypt(buf, 16);
}

int SparkProtocol::set_key(const unsigned char *signed_encrypted_credentials)
{
  unsigned char credentials[40];
  unsigned char hmac[20];

  if (0 != decipher_aes_credentials(core_private_key,
                                    signed_encrypted_credentials,
                                    credentials))
    return 1;

  calculate_ciphertext_hmac(signed_encrypted_credentials, credentials, hmac);

  if (0 == verify_signature(signed_encrypted_credentials + 128,
                            server_public_key,
                            hmac))
  {
    memcpy(key,        credentials,      16);
    memcpy(iv_send,    credentials + 16, 16);
    memcpy(iv_receive, credentials + 16, 16);
    memcpy(salt,       credentials + 32,  8);
    _message_id = *(credentials + 32) << 8 | *(credentials + 33);
    _token = *(credentials + 34);

    unsigned int seed;
    memcpy(&seed, credentials + 35, 4);
    if (handlers.random_seed_from_cloud)
        handlers.random_seed_from_cloud(seed);
    else
        default_random_seed_from_cloud(seed);
    
    return 0;
  }
  else return 2;
}

inline void SparkProtocol::empty_ack(unsigned char *buf,
                                     unsigned char message_id_msb,
                                     unsigned char message_id_lsb)
{
  buf[0] = 0x60; // acknowledgment, no token
  buf[1] = 0x00; // code signifying empty message
  buf[2] = message_id_msb;
  buf[3] = message_id_lsb;

  memset(buf + 4, 12, 12); // PKCS #7 padding

  encrypt(buf, 16);
}

inline void SparkProtocol::coded_ack(unsigned char *buf,
                                     unsigned char token,
                                     unsigned char code,
                                     unsigned char message_id_msb,
                                     unsigned char message_id_lsb)
{
  buf[0] = 0x61; // acknowledgment, one-byte token
  buf[1] = code;
  buf[2] = message_id_msb;
  buf[3] = message_id_lsb;
  buf[4] = token;

  memset(buf + 5, 11, 11); // PKCS #7 padding
  
  encrypt(buf, 16);
}
