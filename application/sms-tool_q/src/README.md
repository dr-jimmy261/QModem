SMS Tool for 3G/4G/5G modem
===================

* sms tool for various of 3g/4g modem
* read sms as raw pdu or decoded text
* support ucs2 decoding

Usage:
----------------

    usage: [options] send phoneNumber message
	    [options] send_raw_pdu pdu
	    [options] recv
	    [options] delete msg_index | all
	    [options] status
	    [options] ussd code
	    [options] at command
	options:
	    -b <baudrate> (default: 115200)
	    -t <seconds> AT command timeout (default: 5)
	    -d <tty device> (default: /dev/ttyUSB0)
	    -D debug (for ussd)
	    -f <date/time format> (for sms/recv)
	    -j json output (for sms/recv)
	    -R use raw input (for ussd)
	    -r use raw output (for ussd and sms/recv)
	    -s <preferred storage> (for sms/recv/status)

Sending uses a raw transaction engine for `/dev/mhi_*` and `/dev/wwan*`
character devices.  These Quectel MHI UCI nodes are not Linux TTYs: termios
and `tcflush()` are skipped, input is drained with `poll()`/`read()`, and the
sender waits for the modem's `>` prompt before writing the PDU and Ctrl-Z.
USB serial devices retain the normal termios setup.  A timeout after Ctrl-Z
is reported as an unknown result; the utility never retries automatically.

Examples:

    sms_tool_q -d /dev/mhi_DUN send 84901234567 "Xin chào"
    sms_tool_q -d /dev/mhi_DUN send_raw_pdu 0011000B...
