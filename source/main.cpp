/* mbed Microcontroller Library
 * Copyright (c) 2006-2013 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "SecurityManager.h"
#include "ble/BLE.h"
#include <events/mbed_events.h>
#include <mbed.h>

/** This example demonstrates all the basic setup required
 *  for pairing and setting up link security both as a central and peripheral
 *
 *  The example is implemented as two classes, one for the peripheral and one
 *  for central inheriting from a common base. They are run in sequence and
 *  require a peer device to connect to. During the peripheral device
 * demonstration a peer device is required to connect. In the central device
 * demonstration this peer device will be scanned for and connected to -
 * therefore it should be advertising with the same address as when it
 * connected.
 *
 *  During the test output is written on the serial connection to monitor its
 *  progress.
 */

static const uint8_t DEVICE_NAME[] = "jenny_SM_device";

/* for demonstration purposes we will store the peer device address
 * of the device that connects to us in the first demonstration
 * so we can use its address to reconnect to it later */
static BLEProtocol::AddressBytes_t peer_address;

/** Base class for both peripheral and central. The same class that provides
 *  the logic for the application also implements the
 * SecurityManagerEventHandler which is the interface used by the Security
 * Manager to communicate events back to the applications. You can provide
 * overrides for a selection of events your application is interested in.
 */
class SMDevice : private mbed::NonCopyable<SMDevice>,
                 public SecurityManager::EventHandler {
public:
    SMDevice(BLE &ble, events::EventQueue &event_queue,
             BLEProtocol::AddressBytes_t &peer_address)
        : _led1(LED1, 0), _ble(ble), _event_queue(event_queue),
          _peer_address(peer_address), _handle(0), _is_connecting(false){};

    virtual ~SMDevice()
    {
        if (_ble.hasInitialized()) {
            _ble.shutdown();
        }
    };

    /** Start BLE interface initialisation */
    void run()
    {
        ble_error_t error;

        /* to show we're running we'll blink every 500ms */
        _event_queue.call_every(500, this, &SMDevice::blink);

        if (_ble.hasInitialized()) {
            printf("Ble instance already initialised.\r\n");
            return;
        }

        /* this will inform us off all events so we can schedule their handling
         * using our event queue */
        _ble.onEventsToProcess(
            makeFunctionPointer(this, &SMDevice::schedule_ble_events));

        /* handle timeouts, for example when connection attempts fail */
        _ble.gap().onTimeout(makeFunctionPointer(this, &SMDevice::on_timeout));

        error = _ble.init(this, &SMDevice::on_init_complete);

        if (error) {
            printf("Error returned by BLE::init.\r\n");
            return;
        }

        /* this will not return until shutdown */
        _event_queue.dispatch_forever();
    };

    /* event handler functions */

    /** Respond to a pairing request. This will be called by the stack
     * when a pairing request arrives and expects the application to
     * call acceptPairingRequest or cancelPairingRequest */
    virtual void pairingRequest(ble::connection_handle_t connectionHandle)
    {
        printf("Pairing requested - authorising\r\n");
        //_ble.securityManager().setOOBDataUsage(connectionHandle, true);
        _ble.securityManager().acceptPairingRequest(connectionHandle);
    }

    /** Inform the application of a successful pairing. Terminate the
     * demonstration. */
    virtual void
    pairingResult(ble::connection_handle_t connectionHandle,
                  SecurityManager::SecurityCompletionStatus_t result)
    {
        if (result == SecurityManager::SEC_STATUS_SUCCESS) {
            printf("Pairing successful\r\n");
        } else {
            printf("Pairing failed, result = %d\r\n", result);
        }
    }

    /** Inform the application of change in encryption status. This will be
     * communicated through the serial port */
    virtual void linkEncryptionResult(ble::connection_handle_t connectionHandle,
                                      ble::link_encryption_t result)
    {
        if (result == ble::link_encryption_t::ENCRYPTED) {
            printf("Link ENCRYPTED\r\n");
        } else if (result == ble::link_encryption_t::ENCRYPTED_WITH_MITM) {
            printf("Link ENCRYPTED_WITH_MITM\r\n");
        } else if (result == ble::link_encryption_t::NOT_ENCRYPTED) {
            printf("Link NOT_ENCRYPTED\r\n");
        }

        /* disconnect in 2 s */
        /**_event_queue.call_in(
            2000, &_ble.gap(),
            &Gap::disconnect, _handle, Gap::REMOTE_USER_TERMINATED_CONNECTION
        );**/
    }

    /** Indicate that the application needs to send legacy pairing OOB data
     * to the peer. */
    virtual void legacyPairingOobGenerated(const ble::address_t *address,
                                           const ble::oob_tk_t *temporaryKey)
    {
        printf("Temporary key: ");
        const uint8_t *tk = temporaryKey->data();
        for (unsigned int i = 0; i < temporaryKey->size(); i++) {
            printf("%u", tk[i]);
        }
        printf("\r\n");
    }

    /** Indicate that the application needs to send secure connections OOB
     * data to the peer. */
    virtual void oobGenerated(const ble::address_t *address,
                              const ble::oob_lesc_value_t *random,
                              const ble::oob_confirm_t *confirm)
    {
        printf("Random num: ");
        const uint8_t *random_num = random->data();
        for (unsigned int i = 0; i < random->size(); i++) {
            printf("%u", random_num[i]);
        }
        printf("\r\n");
        printf("Confirmation: ");
        const uint8_t *confirm_num = confirm->data();
        for (unsigned int i = 0; i < confirm->size(); i++) {
            printf("%u", confirm_num[i]);
        }
        printf("\r\n");
    }

private:
    /** Override to start chosen activity when initialisation completes */
    virtual void start() = 0;

    /** This is called when BLE interface is initialised and starts the
     * demonstration */
    void on_init_complete(BLE::InitializationCompleteCallbackContext *event)
    {
        ble_error_t error;

        if (event->error) {
            printf("Error during the initialisation\r\n");
            return;
        }

        /* This path will be used to store bonding information but will fallback
         * to storing in memory if file access fails (for example due to lack of
         * a filesystem) */
        const char *db_path = "/fs/bt_sec_db";
        /* If the security manager is required this needs to be called before
         * any calls to the Security manager happen. */
        error = _ble.securityManager().init(
            true,  // enableBonding
            false, // requireMITM
            SecurityManager::IO_CAPS_NONE,
            NULL,   // passkey
            false,  // signing
            db_path // Path to the file used to store keys in the filesystem
        );

        if (error) {
            printf("Error during init %d\r\n", error);
            return;
        }

        error = _ble.securityManager().preserveBondingStateOnReset(true);

        if (error) {
            printf("Error during preserveBondingStateOnReset %d\r\n", error);
        }

        /* Tell the security manager to use methods in this class to inform us
         * of any events. Class needs to implement SecurityManagerEventHandler.
         */
        _ble.securityManager().setSecurityManagerEventHandler(this);

        /* print device address */
        Gap::AddressType_t addr_type;
        Gap::Address_t addr;
        _ble.gap().getAddress(&addr_type, addr);

        printf("Device address: %02x:%02x:%02x:%02x:%02x:%02x\r\n", addr[5],
               addr[4], addr[3], addr[2], addr[1], addr[0]);

        uint8_t own_addr[6];
        for (int i = 0; i < 6; i++) {
            own_addr[i] = (uint8_t)addr[i];
        }
        //printf("Own address: %02x:%02x:%02x:%02x:%02x:%02x\r\n", own_addr[5], own_addr[4], own_addr[3], own_addr[2], own_addr[1], own_addr[0]);

        const ble::address_t own_address = ble::address_t(own_addr);

        ble_error_t oob;
        oob = _ble.securityManager().generateOOB(&own_address);
        // printf("generateOOB status = %d\r\n", oob);

        /* when scanning we want to connect to a peer device so we need to
         * attach callbacks that are used by Gap to notify us of events */
        _ble.gap().onConnection(this, &SMDevice::on_connect);
        _ble.gap().onDisconnection(this, &SMDevice::on_disconnect);

        /* start test in 500 ms */
        _event_queue.call_in(500, this, &SMDevice::start);
    };

    /** This is called by Gap to notify the application we connected */
    virtual void
    on_connect(const Gap::ConnectionCallbackParams_t *connection_event) = 0;

    /** This is called by Gap to notify the application we disconnected,
     *  in our case it ends the demonstration. */
    void on_disconnect(const Gap::DisconnectionCallbackParams_t *event)
    {
        printf("Disconnected\r\n");
        _event_queue.break_dispatch();
    };

    /** End demonstration unexpectedly. Called if timeout is reached during
     * advertising, scanning or connection initiation */
    void on_timeout(const Gap::TimeoutSource_t source)
    {
        printf("Unexpected timeout - aborting\r\n");
        _event_queue.break_dispatch();
    };

    /** Schedule processing of events from the BLE in the event queue. */
    void schedule_ble_events(BLE::OnEventsToProcessCallbackContext *context)
    {
        _event_queue.call(mbed::callback(&context->ble, &BLE::processEvents));
    };

    /** Blink LED to show we're running */
    void blink(void)
    {
        _led1 = !_led1;
    };

private:
    DigitalOut _led1;

protected:
    BLE &_ble;
    events::EventQueue &_event_queue;
    BLEProtocol::AddressBytes_t &_peer_address;
    ble::connection_handle_t _handle;
    bool _is_connecting;
};

/** A peripheral device will advertise, accept the connection and request
 * a change in link security. */
class SMDevicePeripheral : public SMDevice {
public:
    SMDevicePeripheral(BLE &ble, events::EventQueue &event_queue,
                       BLEProtocol::AddressBytes_t &peer_address)
        : SMDevice(ble, event_queue, peer_address)
    {
    }

    virtual void start()
    {
        /* Set up and start advertising */

        ble_error_t error;
        GapAdvertisingData advertising_data;

        /* add advertising flags */
        advertising_data.addFlags(GapAdvertisingData::LE_GENERAL_DISCOVERABLE |
                                  GapAdvertisingData::BREDR_NOT_SUPPORTED);

        /* add device name */
        advertising_data.addData(GapAdvertisingData::COMPLETE_LOCAL_NAME,
                                 DEVICE_NAME, sizeof(DEVICE_NAME));

        error = _ble.gap().setAdvertisingPayload(advertising_data);

        if (error) {
            printf("Error during Gap::setAdvertisingPayload\r\n");
            return;
        }

        /* advertise to everyone */
        _ble.gap().setAdvertisingType(
            GapAdvertisingParams::ADV_CONNECTABLE_UNDIRECTED);
        /* how many milliseconds between advertisements, lower interval
         * increases the chances of being seen at the cost of more power */
        _ble.gap().setAdvertisingInterval(20);
        _ble.gap().setAdvertisingTimeout(0);

        error = _ble.gap().startAdvertising();

        if (error) {
            printf("Error during Gap::startAdvertising.\r\n");
            return;
        }

        printf("Please connect to device\r\n");

        /** This tells the stack to generate a pairingRequest event
         * which will require this application to respond before pairing
         * can proceed. Setting it to false will automatically accept
         * pairing. */
        _ble.securityManager().setPairingRequestAuthorisation(true);
    };

    /** This is called by Gap to notify the application we connected,
     *  in our case it immediately requests a change in link security */
    virtual void
    on_connect(const Gap::ConnectionCallbackParams_t *connection_event)
    {
        ble_error_t error;

        /* remember the device that connects to us now so we can connect to it
         * during the next demonstration */
        memcpy(_peer_address, connection_event->peerAddr,
               sizeof(_peer_address));

        /**
        ble_error_t oob;
        oob = _ble.securityManager().generateOOB(_ble.gap().getPeerAddress());
        printf("generateOOB = %d", oob);
        **/

        printf("Connected to: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
               _peer_address[5], _peer_address[4], _peer_address[3],
               _peer_address[2], _peer_address[1], _peer_address[0]);

        /* store the handle for future Security Manager requests */
        _handle = connection_event->handle;

        /* Request a change in link security. This will be done
         * indirectly by asking the master of the connection to
         * change it. Depending on circumstances different actions
         * may be taken by the master which will trigger events
         * which the applications should deal with. */
        error = _ble.securityManager().setLinkSecurity(
            _handle, SecurityManager::SECURITY_MODE_ENCRYPTION_WITH_MITM);

        if (error) {
            printf("Error during SM::setLinkSecurity %d\r\n", error);
            return;
        }
    };
};

int main()
{
    BLE &ble = BLE::Instance();
    events::EventQueue queue;

    while (1) {
        {
            printf("\r\n PERIPHERAL \r\n\r\n");
            SMDevicePeripheral peripheral(ble, queue, peer_address);
            peripheral.run();
        }
    }

    return 0;
}
