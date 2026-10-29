
#include <moodycamel/moodycamel.h>
#include <juce_serialport/juce_serialport.h>

#include "input_events.h"
#include "contracts/dcl/dcl_contracts.h"
#include "contracts/dcl/dcl_contracts_helper.h"
#include "kick_SimulatorInterface.h"

#include "ComConfig.hpp"
#include "source/kick_SurfaceFrameReader.h"

// The serial link to the control-surface microcontroller: compiled wherever juce_serialport has
// a POSIX implementation (Linux, macOS). On the unit it is /dev/ttymxc0; KICK_SERIAL_DEVICE
// names another device (a USB adapter, or the pseudo-terminal of a simulated panel).
#if JUCE_SERIALPORT_POSIX
    #define KICK_SERIAL_LINK 1
#else
    #define KICK_SERIAL_LINK 0
#endif

namespace Controls
{
    std::atomic<bool> halfLevelState{ false };

    ///
    /// Static members...
    static std::array<uint8_t, BUFFER_RX_SIZE> ms_arrui8RxBufferUsb;
    static std::array<uint8_t, BUFFER_RX_SIZE> ms_arrui8TxBufferUsb;
    static SimulatorInterface*                 simInterface{ nullptr };
    static CPktMgr                             ms_objPktMgrUsb{ CPktMgr::EN_COM_CHANNEL_USB, ms_arrui8RxBufferUsb.data(), BUFFER_RX_SIZE, ms_arrui8TxBufferUsb.data(), BUFFER_RX_SIZE, 60 };

    sigslot::signal<CPktMgr::HWMsg_t&> Controls::InputEvents::dispatchHardwareEvent = {};

    ///
    /// IMPL...
    class HwSerialData : private juce::ChangeListener
        , private juce::Thread
        , private juce::AsyncUpdater
        , private juce::Timer
    {
    public:
        HwSerialData()
            : Thread( "Queue" )
        {
            frames.onProblem = []( const std::string& What ) { juce::Logger::writeToLog( "InputEvents: " + juce::String( What ) ); };
            startThread();
        }

        ~HwSerialData()
        {
            stopTimer();
            close();
        }

        void connect()
        {
            // The send thread: close() stops it, so a reconnect() must start it again (it did
            // not, and every frame after a reconnect was queued and never sent).
            if( !isThreadRunning() )
                startThread();
#if KICK_SERIAL_LINK
            // KICK_SERIAL_DEVICE=<absolute path> replaces the unit's port; unset, it is
            // /dev/ttymxc0 as always. Same settings and handshake either way.
            const juce::String overridePath = juce::SystemStats::getEnvironmentVariable( "KICK_SERIAL_DEVICE", {} ).trim();
            const juce::String serialDev    = overridePath.isNotEmpty() ? overridePath : juce::String( "/dev/ttymxc0" );
            const juce::String origin       = overridePath.isNotEmpty() ? "KICK_SERIAL_DEVICE" : "default";

            if( juce::File::isAbsolutePath( serialDev ) && juce::File( serialDev ).exists() )
            {
                juce::StringPairArray portlist = SerialPort::getSerialPortPaths();

                // The unit's own port is listed in /dev; a pseudo-terminal named by
                // KICK_SERIAL_DEVICE (/dev/pts/N, /dev/ttysNNN) is not, and needs no listing.
                if( !portlist.size() && overridePath.isEmpty() )
                    return;

                auto serialDebug = []( juce::String str1, juce::String str2 ) {
                    juce::Logger::writeToLog( str1 + ": " + str2 );
                };
                SerialPortConfig config( 230400, 8, SerialPortConfig::SERIALPORT_PARITY_NONE, SerialPortConfig::STOPBITS_1, SerialPortConfig::FLOWCONTROL_NONE );

                juce::String devPath = serialDev;
                if( sPort )
                {
                    input.reset( nullptr );
                    output.reset( nullptr );
                    sPort.reset( nullptr );
                }
                {
                    CPktMgr::HWMsg_t hwMsg;
                    hwMsg.pktType = CPktMgr::EN_PKT_TYPE_SYSTEM;
                    hwMsg.pktCmd  = contracts::DeviceControlContracts::SysCmd::EN_SYS_COMM_INIT;
                    hwMsg.pktDir  = CPktMgr::EN_PKT_REQUEST;

                    sPort.reset( new SerialPort( devPath, config, serialDebug ) );
                    juce::Logger::writeToLog( "InputEvents: control surface on " + devPath + " (" + origin + "), " + config.toString() + ": " + ( sPort->isOpen() ? "open" : "could not open" ) );
                    frames.reset();
                    missingLogged = false;
                    // From the first open on, watch for a hang-up (the port's reader stops: a USB
                    // adapter unplugged, the other end of a pseudo-terminal closed) and reopen
                    // the port then. /dev/ttymxc0 on the unit does not hang up, so there this
                    // only ever checks.
                    if( sPort->isOpen() && !isTimerRunning() )
                        startTimer( 1000 );
                    input.reset( new SerialPortInputStream( sPort.get() ) );
                    input->addChangeListener( this );
                    input->setNotify( SerialPortInputStream::NOTIFY_ALWAYS );

                    hwMsg.data.push_back( (int)contracts::DeviceControlContracts::SysCommCmd::EN_SYS_COMM_CONNECTED );
                    output.reset( new SerialPortOutputStream( sPort.get() ) );
                    Controls::InputEvents::sendMessageToHardware( hwMsg );
                }
            }
            else
            {
                DBG( "Serial Not Found" );
                if( overridePath.isNotEmpty() && !missingLogged )
                    juce::Logger::writeToLog( "InputEvents: KICK_SERIAL_DEVICE=" + overridePath + " does not exist (an absolute path is needed)" );
                missingLogged = true;
            }
#endif
        }

        void close()
        {
            if( sPort ) sPort->close();
            stopThread( 1000 );
            waitForThreadToExit( 1000 );
        }

        void reconnect()
        {
            close();
            connect();
        }

        void sendMessageToControlSurface( CPktMgr::HWMsg_t& f_HwMsg )
        { // Enqueue in the queue...
            txQueue.enqueue( f_HwMsg );
            //semaphoreQueue.signal();
            triggerAsyncUpdate();
        }

        void messageReceivedFromControlSurface( CPktMgr::HWMsg_t& f_rxMsg )
        {
            juce::String log;
            log += " Data[0] : " + juce::String( f_rxMsg.data[0] ) + " Cmd : " + juce::String( f_rxMsg.pktCmd ) + +" type : " + juce::String( f_rxMsg.pktType );
            juce::Logger::writeToLog( log );

            Controls::InputEvents::dispatchHardwareEvent( f_rxMsg );
        }

    private:
        void handleAsyncUpdate() override { txEvent.signal(); }
        void sendSerialOutData( const std::vector<char>& data )
        {
#if KICK_SERIAL_LINK
            if( !output )
                return;

            juce::MemoryBlock outData;
            uint8_t           f_txSize = data.size();
            //if(f_txSize > 0)
            //{
            //p + (4 int size) + (hw id) + data + \n
            outData.ensureSize( f_txSize );
            //DBG("======TX==Message======");
            for( unsigned int i = 0; i < f_txSize; i++ )
            {
                outData[i] = data[i];
                //DBG("TxData: " << (int)outData[i]);
            }

            output->write( outData.getData(), outData.getSize() );
            output->flush();
            //}
#endif
        }

        void changeListenerCallback( juce::ChangeBroadcaster* ) override
        {
            // Everything that has arrived goes to the frame reader, which keeps a partial frame
            // for the next notification and resynchronises on 'M' 'T' after anything else
            // (kick_SurfaceFrameReader.h). Before, a read that did not start with a complete
            // frame was dropped whole: a frame split over two reads was lost with the frame
            // after it, and garbage lost every frame after it in that read.
            const auto available = (int)input->getTotalLength();
            if( available <= 0 )
                return;
            juce::HeapBlock<uint8_t> buffer( (size_t)available );
            const int                got = input->read( buffer.get(), available );
            if( got <= 0 )
                return;
            frames.push( buffer.get(), (size_t)got, [this]( CPktMgr::HWMsg_t& rxMsg ) {
                if( rxMsg.pktDir != CPktMgr::enumPacketDir::EN_PKT_REQUEST )
                    return; // a response (an acknowledgement of the app's message): ignored, as before
                if( contracts::DclContractsHelper::isButtonMessage( rxMsg ) )
                {
                    DBG( "Button state = " << contracts::DclContractsHelper::getButtonState( rxMsg ) );
                    if( ( contracts::DeviceControlContracts::EN_SHIFT == contracts::DclContractsHelper::getButtonCode( rxMsg ) ) )
                        contracts::DclContractsHelper::setShiftPressed( contracts::DclContractsHelper::getButtonState( rxMsg ) != contracts::DeviceControlContracts::BTN_RELEASED );
                }
                messageReceivedFromControlSurface( rxMsg );
            } );
        }

        void timerCallback() override
        {
#if KICK_SERIAL_LINK
            // The port hung up: its reader stopped. Reopen it (connect() also sends the first
            // handshake frame again), once a second until it is back.
            if( input != nullptr && input->isReading() && sPort != nullptr && sPort->isOpen() )
                return;
            if( !hangUpLogged )
                juce::Logger::writeToLog( "InputEvents: the control surface port hung up; reopening it" );
            hangUpLogged = true;
            input.reset();
            output.reset();
            sPort.reset();
            connect();
            if( input != nullptr && input->isReading() )
            {
                juce::Logger::writeToLog( "InputEvents: the control surface port is open again" );
                hangUpLogged = false;
            }
#endif
        }

        void run() override
        {
            static CPktMgr::HWMsg_t  item;
            static std::vector<char> txData;
            while( !threadShouldExit() )
            {
                txEvent.wait( 1000 );
                //semaphoreQueue.wait( 1 );

                while( txQueue.try_dequeue( item ) && !threadShouldExit() )
                {
#if KICK_SERIAL_LINK
                    txData.clear();
                    ms_objPktMgrUsb.BuildAndTransmitPacket( item, txData );
                    sendSerialOutData( txData );

                    item.data.clear();
#endif
                }
            } //DBG("Out of While");
        }

    private:
        juce::WaitableEvent txEvent;

        std::unique_ptr<SerialPort>             sPort;
        std::unique_ptr<SerialPortInputStream>  input;
        std::unique_ptr<SerialPortOutputStream> output;

        moodycamel::ConcurrentQueue<CPktMgr::HWMsg_t> txQueue;

        SurfaceFrameReader frames{ ms_objPktMgrUsb, BUFFER_RX_SIZE };
        bool               missingLogged{ false }, hangUpLogged{ false };
    };
    static std::unique_ptr<HwSerialData> _FPPort;

    /// <summary>
    ///
    /// Class Controls::InputEvents
    ///
    /// </summary>

    void Controls::InputEvents::initialize()
    {
#ifdef SIM_BUILD
        //creating multiple instance of SimulatorInterface using factoty is somehow clearing the connected signals need to find the reason
        simInterface = Factory<SimulatorInterface>::makeObject( 0 );
        simInterface->hwInputReceived.connect( []( int id, const std::vector<char>& data ) {
            Controls::InputEvents::dispatchHardwareEvent( id, data );
        } );
#else
        if( !_FPPort )
        {
            _FPPort.reset( new HwSerialData );
            ms_objPktMgrUsb.Init();
        }
        _FPPort->connect();
        // Send Rebooted command to GD
#endif
    }

    void Controls::InputEvents::reconnect()
    {
        if( _FPPort ) _FPPort->reconnect();
    }

    void Controls::InputEvents::shutdown()
    {
        _FPPort.reset();
    }

    void Controls::InputEvents::messageReceived( CPktMgr::HWMsg_t& f_rxMsg )
    {
        if( _FPPort ) _FPPort->messageReceivedFromControlSurface( f_rxMsg );
    }

    void Controls::InputEvents::sendMessageToHardware( CPktMgr::HWMsg_t& f_HwMsg )
    {
        if( f_HwMsg.pktType == CPktMgr::enumPacketType::EN_PKT_TYPE_SYSTEM )
        {
            if( f_HwMsg.pktCmd == contracts::DeviceControlContracts::EN_SYS_RECONNECT )
            {
                reconnect();
                return;
            }
            if( f_HwMsg.pktCmd == contracts::DeviceControlContracts::EN_SYS_SHUTDOWN )
            {
                shutdown();
                return;
            }
        }

#ifdef SIM_BUILD
        if( simInterface ) simInterface->sendMessageToHW( f_HwMsg );
#endif
        if( _FPPort ) _FPPort->sendMessageToControlSurface( f_HwMsg );
        else
        {
            juce::Logger::outputDebugString( "Null Serial Port " );
        }
    }
}
