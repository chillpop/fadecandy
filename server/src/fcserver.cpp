/*
 * Open Pixel Control server for Fadecandy
 * 
 * Copyright (c) 2013 Micah Elizabeth Scott
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "fcserver.h"
#include "usbdevice.h"
#include "fcdevice.h"
#include "enttecdmxdevice.h"
#include <ctype.h>
#include <iostream>


FCServer::FCServer(rapidjson::Document &config)
    : mListen(config["listen"]),
      mColor(config["color"]),
      mDevices(config["devices"]),
      mVerbose(config["verbose"].IsTrue()),
      mListenAddr(0),
      mOPCSink(cbMessage, this, mVerbose),
      mUSBHotplugThread(0),
      mUSB(0)
{
    /*
     * Parse the listen [host, port] list.
     */

    if (mListen.IsArray() && mListen.Size() == 2) {
        const Value &host = mListen[0u];
        const Value &port = mListen[1];
        const char *hostStr = 0;

        if (host.IsString()) {
            hostStr = host.GetString();
        } else if (!host.IsNull()) {
            mError << "Hostname in 'listen' must be null (any) or a hostname string.\n";
        }

        if (port.IsUint()) {
            mListenAddr = OPCSink::newAddr(hostStr, port.GetUint());
            if (!mListenAddr) {
                mError << "Failed to resolve hostname '" << hostStr << "'\n";
            }
        } else {
            mError << "The 'listen' port must be an integer.\n";
        }
    } else {
        mError << "The required 'listen' configuration key must be a [host, port] list.\n";
    }

    /*
     * Minimal validation on 'devices'
     */

    if (!mDevices.IsArray()) {
        mError << "The required 'devices' configuration key must be an array.\n";
    }
}

FCServer::~FCServer()
{
    if (mListenAddr) {
        OPCSink::freeAddr(mListenAddr);
    }
}

void FCServer::start(libusb_context *usb)
{
    mOPCSink.start(mListenAddr);
    startUSB(usb);
}

void FCServer::startUSB(libusb_context *usb)
{   
    mUSB = usb;

    // Enumerate all attached devices, and get notified of hotplug events
    libusb_hotplug_register_callback(mUSB,
        libusb_hotplug_event(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                             LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
        LIBUSB_HOTPLUG_ENUMERATE,
        LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY,
        cbHotplug, this, 0);

    // On platforms without real USB hotplug, emulate it with a polling thread
    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        mUSBHotplugThread = new tthread::thread(usbHotplugThreadFunc, this);
    }
}

void FCServer::cbMessage(OPCSink::Message &msg, void *context)
{
    /*
     * Broadcast the OPC message to all configured devices.
     */

    FCServer *self = static_cast<FCServer*>(context);
    self->mEventMutex.lock();

    for (std::vector<USBDevice*>::iterator i = self->mUSBDevices.begin(), e = self->mUSBDevices.end(); i != e; ++i) {
        USBDevice *dev = *i;
        dev->writeMessage(msg);
    }

    self->mEventMutex.unlock();
}

int FCServer::cbHotplug(libusb_context *ctx, libusb_device *device, libusb_hotplug_event event, void *user_data)
{
    FCServer *self = static_cast<FCServer*>(user_data);

    self->mEventMutex.lock();

    if (event & LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        self->usbDeviceArrived(device);
    }
    if (event & LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
        self->usbDeviceLeft(device);
    }

    self->mEventMutex.unlock();
    return false;
}

void FCServer::usbDeviceArrived(libusb_device *device)
{
    /*
     * New USB device. Is this a device we recognize?
     */

    USBDevice *dev;

    if (FCDevice::probe(device)) {
        dev = new FCDevice(device, mVerbose);

    } else if (EnttecDMXDevice::probe(device)) {
        dev = new EnttecDMXDevice(device, mVerbose);

    } else {
        return;
    }

    int r = dev->open();
    if (r < 0) {
        if (mVerbose) {
            switch (r) {

                // Errors that may occur transiently while WinUSB is installing...
                #ifdef _WIN32
                case LIBUSB_ERROR_NOT_FOUND:
                case LIBUSB_ERROR_NOT_SUPPORTED:
                    std::clog << "Waiting for Windows to install " << dev->getName() << " driver. This may take a moment...\n";
                    break;
                #endif

                default:
                    std::clog << "Error opening " << dev->getName() << ": " << libusb_strerror(libusb_error(r)) << "\n";
                    break;
            }
        }
        delete dev;
        return;
    }

    if (!dev->probeAfterOpening()) {
        // We were mistaken, this device isn't actually one we want.
        delete dev;
        return;
    }

    for (unsigned i = 0; i < mDevices.Size(); ++i) {
        if (dev->matchConfiguration(mDevices[i])) {
            // Found a matching configuration for this device. We're keeping it!

            dev->writeColorCorrection(mColor);
            mUSBDevices.push_back(dev);

            if (mVerbose) {
                std::clog << "USB device " << dev->getName() << " attached.\n";
            }
            return;
        }
    }

    if (mVerbose) {
        std::clog << "USB device " << dev->getName() << " has no matching configuration. Not using it.\n";
    }
    delete dev;
}

void FCServer::usbDeviceLeft(libusb_device *device)
{
    /*
     * Is this a device we recognize? If so, delete it.
     */

    for (std::vector<USBDevice*>::iterator i = mUSBDevices.begin(), e = mUSBDevices.end(); i != e; ++i) {
        USBDevice *dev = *i;
        if (dev->getDevice() == device) {
            usbDeviceLeft(i);
            break;
        }
    }
}

void FCServer::usbDeviceLeft(std::vector<USBDevice*>::iterator iter)
{
    USBDevice *dev = *iter;
    if (mVerbose) {
        std::clog << "USB device " << dev->getName() << " removed.\n";
    }
    mUSBDevices.erase(iter);
    delete dev;
}

void FCServer::mainLoop()
{
    for (;;) {
        int err = libusb_handle_events_completed(mUSB, 0);
        if (err) {
            std::clog << "Error handling USB events: " << libusb_strerror(libusb_error(err)) << "\n";
            // Sometimes this happens on Windows during normal operation if we're queueing a lot of output URBs. Meh.
        }

        // Flush completed transfers
        mEventMutex.lock();
        for (std::vector<USBDevice*>::iterator i = mUSBDevices.begin(), e = mUSBDevices.end(); i != e; ++i) {
            USBDevice *dev = *i;
            dev->flush();
        }
        mEventMutex.unlock();
    }
}

bool FCServer::usbHotplugPoll()
{
    /*
     * For platforms without libusbx hotplug support,
     * see if we can fake it by polling for new devices. This can
     * happen on a different thread, and it probably should.
     * Returns true on success.
     */

    libusb_device **list;
    ssize_t listSize;

    listSize = libusb_get_device_list(mUSB, &list);
    if (listSize < 0) {
        std::clog << "Error polling for USB devices: " << libusb_strerror(libusb_error(listSize)) << "\n";
        return false;
    }

    // Take the lock after get_device_list completes
    mEventMutex.lock();

    // Look for devices that were added
    for (ssize_t listItem = 0; listItem < listSize; ++listItem) {
        bool isNew = true;

        for (std::vector<USBDevice*>::iterator i = mUSBDevices.begin(), e = mUSBDevices.end(); i != e; ++i) {
            USBDevice *dev = *i;
            if (dev->getDevice() == list[listItem]) {
                isNew = false;
            }
        }

        if (isNew) {
            usbDeviceArrived(list[listItem]);
        }
    }

    // Look for devices that were removed
    for (std::vector<USBDevice*>::iterator i = mUSBDevices.begin(), e = mUSBDevices.end(); i != e; ++i) {
        USBDevice *dev = *i;
        libusb_device *usbdev = dev->getDevice();
        bool isRemoved = true;

        for (ssize_t listItem = 0; listItem < listSize; ++listItem) {
            if (list[listItem] == usbdev) {
                isRemoved = false;
            }
        }

        if (isRemoved) {
            usbDeviceLeft(i);
        }
    }

    mEventMutex.unlock();
    libusb_free_device_list(list, true);
    return true;
}

void FCServer::usbHotplugThreadFunc(void *arg)
{
    FCServer *self = (FCServer*) arg;

    while (self->usbHotplugPoll()) {
        tthread::this_thread::sleep_for(tthread::chrono::seconds(1));
    }
}
