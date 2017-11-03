// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/IOS/USB/USB_HID/HIDv5.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/USB/Common.h"

namespace IOS
{
namespace HLE
{
namespace Device
{
constexpr u32 USBV5_VERSION = 0x50001;

USB_HIDv5::~USB_HIDv5() = default;

IPCCommandResult USB_HIDv5::IOCtl(const IOCtlRequest& request)
{
  request.Log(GetDeviceName(), LogTypes::IOS_USB);
  switch (request.request)
  {
  case USB::IOCTL_USBV5_GETVERSION:
    Memory::Write_U32(USBV5_VERSION, request.buffer_out);
    return GetDefaultReply(IPC_SUCCESS);
  case USB::IOCTL_USBV5_GETDEVICECHANGE:
    return GetDeviceChange(request);
  case USB::IOCTL_USBV5_SHUTDOWN:
    return Shutdown(request);
  case USB::IOCTL_USBV5_GETDEVPARAMS:
    return HandleDeviceIOCtl(request, [&](auto& device) { return GetDeviceInfo(device, request); });
  case USB::IOCTL_USBV5_ATTACHFINISH:
    return GetDefaultReply(IPC_SUCCESS);
  case USB::IOCTL_USBV5_SUSPEND_RESUME:
    return HandleDeviceIOCtl(request, [&](auto& device) { return SuspendResume(device, request); });
  case USB::IOCTL_USBV5_CANCELENDPOINT:
    return HandleDeviceIOCtl(request,
                             [&](auto& device) { return CancelEndpoint(device, request); });
  default:
    request.DumpUnknown(GetDeviceName(), LogTypes::IOS_USB, LogTypes::LERROR);
    return GetDefaultReply(IPC_SUCCESS);
  }
}

IPCCommandResult USB_HIDv5::IOCtlV(const IOCtlVRequest& request)
{
  request.DumpUnknown(GetDeviceName(), LogTypes::IOS_USB);
  switch (request.request)
  {
  // TODO: HIDv5 seems to be able to queue transfers depending on the transfer length (unlike VEN).
  case USB::IOCTLV_USBV5_CTRLMSG:
  case USB::IOCTLV_USBV5_INTRMSG:
  {
    // IOS does not check the number of vectors, but let's do that to avoid out-of-bounds reads.
    if (request.in_vectors.size() + request.io_vectors.size() != 2)
      return GetDefaultReply(IPC_EINVAL);

    std::lock_guard<std::mutex> lock{m_usbv5_devices_mutex};
    USBV5Device* device = GetUSBV5Device(request.in_vectors[0].address);
    if (!device)
      return GetDefaultReply(IPC_EINVAL);
    auto host_device = GetDeviceById(device->host_id);
    host_device->Attach(device->interface_number);
    return HandleTransfer(host_device, request.request,
                          [&, this]() { return SubmitTransfer(*device, *host_device, request); });
  }
  default:
    return GetDefaultReply(IPC_EINVAL);
  }
}

s32 USB_HIDv5::SubmitTransfer(USBV5Device& device, USB::Device& host_device,
                              const IOCtlVRequest& ioctlv)
{
  switch (ioctlv.request)
  {
  case USB::IOCTLV_USBV5_CTRLMSG:
    return host_device.SubmitTransfer(std::make_unique<USB::V5CtrlMessage>(m_ios, ioctlv));
  case USB::IOCTLV_USBV5_INTRMSG:
  {
    auto message = std::make_unique<USB::V5IntrMessage>(m_ios, ioctlv);

    // Unlike VEN, the endpoint is determined by the value at 8-12.
    // If it's non-zero, HID submits the request to the interrupt OUT endpoint.
    // Otherwise, the request is submitted to the IN endpoint.
    AdditionalDeviceData* data = &m_additional_device_data[&device - m_usbv5_devices.data()];
    if (Memory::Read_U32(ioctlv.in_vectors[0].address + 8) != 0)
      message->endpoint = data->interrupt_out_endpoint;
    else
      message->endpoint = data->interrupt_in_endpoint;

    return host_device.SubmitTransfer(std::move(message));
  }
  default:
    return IPC_EINVAL;
  }
}

IPCCommandResult USB_HIDv5::CancelEndpoint(USBV5Device& device, const IOCtlRequest& request)
{
  // FIXME: Unlike VEN, there are 3 valid values for the endpoint,
  //        which determine the endpoint address that gets passed to the backend.
  //        Valid values: 0 (control, endpoint 0), 1 (interrupt IN) and 2 (interrupt OUT)
  //        This ioctl also cancels all queued transfers with return code -7022.
  request.DumpUnknown(GetDeviceName(), LogTypes::IOS_USB);
  const u8 endpoint = static_cast<u8>(Memory::Read_U32(request.buffer_in + 8));
  GetDeviceById(device.host_id)->CancelTransfer(endpoint);
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult USB_HIDv5::GetDeviceInfo(USBV5Device& device, const IOCtlRequest& request)
{
  if (request.buffer_out == 0 || request.buffer_out_size != 0x60)
    return GetDefaultReply(IPC_EINVAL);

  const std::shared_ptr<USB::Device> host_device = GetDeviceById(device.host_id);
  const u8 alt_setting = Memory::Read_U8(request.buffer_in + 8);

  Memory::Memset(request.buffer_out, 0, request.buffer_out_size);
  Memory::Write_U32(Memory::Read_U32(request.buffer_in), request.buffer_out);
  Memory::Write_U32(1, request.buffer_out + 4);

  USB::DeviceDescriptor device_descriptor = host_device->GetDeviceDescriptor();
  device_descriptor.Swap();
  Memory::CopyToEmu(request.buffer_out + 36, &device_descriptor, sizeof(device_descriptor));

  // Just like VEN, HIDv5 only cares about the first configuration.
  USB::ConfigDescriptor config_descriptor = host_device->GetConfigurations()[0];
  config_descriptor.Swap();
  Memory::CopyToEmu(request.buffer_out + 56, &config_descriptor, sizeof(config_descriptor));

  std::vector<USB::InterfaceDescriptor> interfaces = host_device->GetInterfaces(0);
  auto it = std::find_if(interfaces.begin(), interfaces.end(), [&](const auto& interface) {
    return interface.bInterfaceNumber == device.interface_number &&
           interface.bAlternateSetting == alt_setting;
  });
  if (it == interfaces.end())
    return GetDefaultReply(IPC_EINVAL);
  it->Swap();
  Memory::CopyToEmu(request.buffer_out + 68, &*it, sizeof(*it));

  auto endpoints = host_device->GetEndpoints(0, it->bInterfaceNumber, it->bAlternateSetting);
  for (auto& endpoint : endpoints)
  {
    constexpr u8 ENDPOINT_INTERRUPT = 0b11;
    constexpr u8 ENDPOINT_IN = 0x80;
    if (endpoint.bmAttributes == ENDPOINT_INTERRUPT)
    {
      const bool is_in_endpoint = (endpoint.bEndpointAddress & ENDPOINT_IN) != 0;

      AdditionalDeviceData* data = &m_additional_device_data[&device - m_usbv5_devices.data()];
      if (is_in_endpoint)
        data->interrupt_in_endpoint = endpoint.bEndpointAddress;
      else
        data->interrupt_out_endpoint = endpoint.bEndpointAddress;

      const u32 offset = is_in_endpoint ? 80 : 88;
      endpoint.Swap();
      Memory::CopyToEmu(request.buffer_out + offset, &endpoint, sizeof(endpoint));
    }
  }

  return GetDefaultReply(IPC_SUCCESS);
}

bool USB_HIDv5::ShouldAddDevice(const USB::Device& device) const
{
  // XXX: HIDv5 opens /dev/usb/usb with mode 3 (which is likely HID_CLASS),
  //      unlike VEN (which opens it with mode 0xff). But is this really correct?
  constexpr u8 HID_CLASS = 0x03;
  return device.HasClass(HID_CLASS);
}
}  // namespace Device
}  // namespace HLE
}  // namespace IOS
