/*
* Copyright (C) 2016 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <cutils/ashmem.h>

#include <hidl/IServiceManager.h>
#include <hwbinder/IInterface.h>
#include <hwbinder/IPCThreadState.h>
#include <hwbinder/ProcessState.h>
#include <utils/Looper.h>
#include <utils/StrongPointer.h>
#include <iostream>
#include <thread>

#include <../common/MessageQueue.h>
#include <android/hardware/benchmarks/msgq/1.0/IBenchmarkMsgQ.h>

// libutils:
using android::Looper;
using android::LooperCallback;
using android::OK;
using android::sp;

// libhwbinder:
using android::hardware::defaultServiceManager;
using android::hardware::IInterface;
using android::hardware::IPCThreadState;
using android::hardware::Parcel;
using android::hardware::ProcessState;
using android::hardware::Return;
using android::hardware::Void;

// Standard library
using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::unique_ptr;
using std::vector;

// libhidl
using android::hardware::kSynchronizedReadWrite;
using android::hardware::MQFlavor;

// Generated HIDL files

using android::hardware::benchmarks::msgq::V1_0::IBenchmarkMsgQ;

/*
 * All benchmark test will per performed on a FMQ of size kQueueSize
 */
const size_t kQueueSize = 16 * 1024;

/*
 * The various packet sizes used are as follows.
 */
enum PacketSizes {
  kPacketSize64 = 64,
  kPacketSize128 = 128,
  kPacketSize256 = 256,
  kPacketSize512 = 512,
  kPacketSize1024 = 1024
};

/*
 * This is the size of ashmem region that will be created for the FMQ.
 */

const size_t kAshmemSize = 20 * 1024;

const char kServiceName[] =
    "android.hardware.benchmarks.msgq@1.0::IBenchmarkMsgQ";

namespace {
/*
 * This method writes numIter packets into the fmsg_queue_outbox_ queue
 * and notes the time before each write in the time_data_ array. It will
 * be used to calculate the average server to client write to read delay.
 */
template <MQFlavor flavor>
void QueueWriter(android::hardware::MessageQueue<uint8_t, flavor>*
                 fmsg_queue_outbox_, int64_t* time_data_, uint32_t numIter) {
  uint8_t data[kPacketSize64];
  uint32_t num_writes = 0;

  while (num_writes < numIter) {
    do {
      time_data_[num_writes] =
          std::chrono::high_resolution_clock::now().time_since_epoch().count();
    } while (fmsg_queue_outbox_->write(data, kPacketSize64) == false);
    num_writes++;
  }
}

/*
 * The method reads a packet from the inbox queue and writes the same
 * into the outbox queue. The client will calculate the average time taken
 * for each iteration which consists of two write and two read operations.
 */
template <MQFlavor flavor>
void QueuePairReadWrite(
    android::hardware::MessageQueue<uint8_t, flavor>* fmsg_queue_inbox_,
    android::hardware::MessageQueue<uint8_t, flavor>* fmsg_queue_outbox_,
    uint32_t numIter) {
  uint8_t data[kPacketSize64];
  uint32_t num_round_trips = 0;

  while (num_round_trips < numIter) {
    while (fmsg_queue_inbox_->read(data, kPacketSize64) == false)
      ;
    while (fmsg_queue_outbox_->write(data, kPacketSize64) == false)
      ;
    num_round_trips++;
  }
}
class BinderCallback : public LooperCallback {
 public:
  BinderCallback() {}
  ~BinderCallback() override {}

  int handleEvent(int /* fd */, int /* events */, void* /* data */) override {
    IPCThreadState::self()->handlePolledCommands();
    return 1;  // Continue receiving callbacks.
  }
};

class BenchmarkMsgQ : public IBenchmarkMsgQ {
 public:
  BenchmarkMsgQ()
      : fmsg_queue_inbox_(nullptr),
        fmsg_queue_outbox_(nullptr),
        time_data_(nullptr) {}
  virtual ~BenchmarkMsgQ() {
    if (fmsg_queue_inbox_) delete fmsg_queue_inbox_;
    if (fmsg_queue_outbox_) delete fmsg_queue_outbox_;
    if (time_data_) delete[] time_data_;
  }
  virtual Return<void> benchmarkPingPong(uint32_t numIter) {
    std::thread(QueuePairReadWrite<kSynchronizedReadWrite>, fmsg_queue_inbox_,
                fmsg_queue_outbox_, numIter)
        .detach();
    return Void();
  }
  virtual Return<void> benchmarkServiceWriteClientRead(uint32_t numIter) {
    if (time_data_) delete[] time_data_;
    time_data_ = new int64_t[numIter];
    std::thread(QueueWriter<kSynchronizedReadWrite>, fmsg_queue_outbox_,
                time_data_, numIter).detach();
    return Void();
  }
  // TODO:: Change callback argument to bool.
  virtual Return<int32_t> requestWrite(int count) {
    uint8_t* data = new uint8_t[count];
    for (int i = 0; i < count; i++) {
      data[i] = i;
    }
    if (fmsg_queue_outbox_->write(data, count)) {
      delete[] data;
      return count;
    }
    delete[] data;
    return 0;
  }
  // TODO:: Change callback argument to bool.
  virtual Return<int32_t> requestRead(int count) {
    uint8_t* data = new uint8_t[count];
    if (fmsg_queue_inbox_->read(data, count)) {
      delete[] data;
      return count;
    }
    delete[] data;
    return 0;
  }
  /*
   * This method is used by the client to send the server timestamps to
   * calculate the server to client write to read delay.
   */
  virtual Return<void> sendTimeData(
      const android::hardware::hidl_vec<int64_t>& client_rcv_time_array) {
    int64_t accumulated_time = 0;
    for (uint32_t i = 0; i < client_rcv_time_array.size(); i++) {
      std::chrono::time_point<std::chrono::high_resolution_clock>
          client_rcv_time((std::chrono::high_resolution_clock::duration(
              client_rcv_time_array[i])));
      std::chrono::time_point<std::chrono::high_resolution_clock>
          server_send_time(
              (std::chrono::high_resolution_clock::duration(time_data_[i])));
      accumulated_time += static_cast<int64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(client_rcv_time -
                                                               server_send_time)
              .count());
    }
    accumulated_time /= client_rcv_time_array.size();
    cout << "Average service to client write to read delay::"
         << accumulated_time << "ns" << endl;
    return Void();
  }

  /*
   * This method requests the service to configure the client's outbox queue.
   */
  virtual Return<void> configureClientOutboxSyncReadWrite(
      IBenchmarkMsgQ::configureClientOutboxSyncReadWrite_cb callback) {
    int ashmemFd =
        ashmem_create_region("MessageQueueClientOutbox", kAshmemSize);
    ashmem_set_prot_region(ashmemFd, PROT_READ | PROT_WRITE);
    if (fmsg_queue_inbox_) delete fmsg_queue_inbox_;
    native_handle_t* mq_handle = native_handle_create(1 /* numFds */,
                                                      0 /* numInts */);
    if (!mq_handle) {
          ALOGE("Unable to create native_handle_t");
          callback(-1 /* ret */, android::hardware::MQDescriptorSync(
                       std::vector<android::hardware::GrantorDescriptor>(),
                       nullptr /* nhandle */, 0 /* size */));
          return Void();
    }

    mq_handle->data[0] = ashmemFd;

    android::hardware::MQDescriptorSync desc(kQueueSize, mq_handle,
                                                            sizeof(uint8_t));

    fmsg_queue_inbox_ = new android::hardware::MessageQueue<uint8_t,
                      kSynchronizedReadWrite>(desc);
    if (fmsg_queue_inbox_ == nullptr) {
      callback(-1 /* ret */, android::hardware::MQDescriptorSync(
                       std::vector<android::hardware::GrantorDescriptor>(),
                       nullptr /* nhandle */, 0 /* size */));
    } else {
      callback(0 /* ret */, desc);
    }
    return Void();
  }
  /*
   * This method requests the service to configure the client's inbox queue.
   */
  virtual Return<void> configureClientInboxSyncReadWrite(
      IBenchmarkMsgQ::configureClientInboxSyncReadWrite_cb callback) {
    int ashmemFd = ashmem_create_region("MessageQueueClientInbox", kAshmemSize);
    ashmem_set_prot_region(ashmemFd, PROT_READ | PROT_WRITE);

    if (fmsg_queue_outbox_) delete fmsg_queue_outbox_;
    native_handle_t* mq_handle = native_handle_create(1, 0);
    if (!mq_handle) {
          ALOGE("Unable to create native_handle_t");
          callback(-1 /* ret */, android::hardware::MQDescriptorSync(
                       std::vector<android::hardware::GrantorDescriptor>(),
                       nullptr /* nhandle */, 0 /* size */));
          return Void();
    }

    mq_handle->data[0] = ashmemFd;

    android::hardware::MQDescriptorSync desc(kQueueSize, mq_handle,
                                                            sizeof(uint8_t));

    fmsg_queue_outbox_ = new android::hardware::MessageQueue<uint8_t,
                       kSynchronizedReadWrite>(desc);
    if (fmsg_queue_outbox_ == nullptr) {
      callback(-1 /* ret */, android::hardware::MQDescriptorSync(
                       std::vector<android::hardware::GrantorDescriptor>(),
                       nullptr /* nhandle */, 0 /* size */));
    } else {
      callback(0 /* ret */, desc);
    }
    return Void();
  }

  android::hardware::MessageQueue<uint8_t, kSynchronizedReadWrite>*
      fmsg_queue_inbox_;
  android::hardware::MessageQueue<uint8_t, kSynchronizedReadWrite>*
      fmsg_queue_outbox_;
  int64_t* time_data_;
};

int Run() {
  android::sp<BenchmarkMsgQ> service = new BenchmarkMsgQ;
  sp<Looper> looper(Looper::prepare(0 /* opts */));
  int binder_fd = -1;
  ProcessState::self()->setThreadPoolMaxThreadCount(0);
  IPCThreadState::self()->disableBackgroundScheduling(true);
  IPCThreadState::self()->setupPolling(&binder_fd);
  if (binder_fd < 0) return -1;

  sp<BinderCallback> cb(new BinderCallback);
  if (looper->addFd(binder_fd, Looper::POLL_CALLBACK, Looper::EVENT_INPUT, cb,
                    nullptr) != 1) {
    ALOGE("Failed to add binder FD to Looper");
    return -1;
  }
  service->registerAsService(kServiceName);

  ALOGI("Entering loop");
  while (true) {
    const int result = looper->pollAll(-1 /* timeoutMillis */);
  }
  return 0;
}

}  // namespace

int main(int /* argc */, char* /* argv */ []) { return Run(); }