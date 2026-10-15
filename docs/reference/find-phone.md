# Find phone

Settings > Bluetooth shows a Ping Phone row beneath the connected phone when
the phone app advertises protocol capability bit 29 (`find_phone_support`).
The row reads the cached system session capabilities, so it stays while the
app briefly disconnects.

## System messages

Both messages use the System Message endpoint (`0x12`,
`fw/kernel/system_message.h`) and carry no payload beyond the zero byte and
the message type.

| Message         | Type   | Direction     |
| --------------- | ------ | ------------- |
| Find phone      | `0x14` | watch → phone |
| Stop find phone | `0x15` | watch → phone |

On Find phone, the phone plays a sound for about five seconds, even when it is
silenced. The row shows that it is pinging for as long. Selecting the row again
during that time sends Stop find phone, and the phone stops the sound early.
The phone doesn't respond to either message.
