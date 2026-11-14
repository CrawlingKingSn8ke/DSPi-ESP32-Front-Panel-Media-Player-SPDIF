/*
 * bridge_config.h - every tunable number in the bridge, in one place.
 *
 * Each constant carries its unit in its name and a comment giving the reason
 * for its value, with the spec section it comes from. "Link spec" means
 * spec/dspi_link_protocol_spec.md; "bridge spec" means
 * spec/esp32_bridge_spec.md. Change a value here, never at the point of use.
 *
 * This header is portable: it includes nothing from ESP-IDF, so host tests
 * can use it too.
 */

#ifndef BRIDGE_CONFIG_H
#define BRIDGE_CONFIG_H

/* ---------------------------------------------------------------------------
 * Link protocol (Link spec 3, 4, 7, 8; bridge spec 8)
 * ------------------------------------------------------------------------- */

/* The Link port: decimal of the DSPi's USB vendor id 0x2E8B (Link spec 3.1). */
#define LINK_TCP_PORT 11915

/* Port 80 serves the setup page on the setup network and a redirect to
 * LINK_TCP_PORT on the home network (bridge spec 8.8). */
#define HTTP_REDIRECT_PORT 80

/* The protocol minimum (Link spec 4); enough for a FWDATA chunk of 16 KB. */
#define LINK_MAX_FRAME_BYTES 16384

/* The protocol minimum (Link spec 7.2); the 6136-byte V32 bulk blob fits. */
#define LINK_MAX_PAYLOAD_BYTES 8192

/* Commands one session may have queued per device (bridge spec 8.1). The
 * UART runs one at a time, so a deeper queue would only add latency. */
#define LINK_MAX_INFLIGHT 4

/* Highest poll rate a session may ask for (bridge spec 8.1). */
#define LINK_POLL_MAX_HZ 20

/* Poll budget in bytes per second is the baud rate divided by this: 40,000
 * at 1 Mbaud, leaving over half the link for commands and notifications
 * (Link spec 9.2, bridge spec 7.7). */
#define LINK_POLL_BUDGET_BAUD_DIVISOR 25

/* Simultaneous sessions (bridge spec 8.2). Each session needs buffers, so
 * boards with PSRAM allow more. */
#define LINK_MAX_SESSIONS_WITHOUT_PSRAM 4
#define LINK_MAX_SESSIONS_WITH_PSRAM 8

/* Frames waiting to be sent to one session before notifications for it are
 * dropped and a RESYNC follows (bridge spec 8.2). */
#define LINK_SESSION_QUEUE_FRAMES 32

/* Keepalive (Link spec 4 and 5). */
#define LINK_PING_INTERVAL_MS 10000
#define LINK_PING_TIMEOUT_MS 30000

/* A session whose socket stays unwritable this long is closed with code
 * 1011 (Link spec 5). */
#define LINK_UNWRITABLE_TIMEOUT_MS 10000

/* A session that has not authenticated by now is closed with code 4001
 * (Link spec 5). */
#define LINK_AUTH_TIMEOUT_MS 30000

/* SETs older than this no longer claim a notification's origin (Link spec
 * 8.3, bridge spec 8.4). */
#define LINK_ORIGIN_WINDOW_MS 1000

/* An offline device keeps its handle this long before device.removed
 * (Link spec 7.4). */
#define LINK_OFFLINE_HANDLE_RETAIN_MS 60000

/* Device names remembered by serial (bridge spec 8.7). A bridge rarely
 * meets more than a few DSPis; 8 keeps the saved list to one 648-byte NVS
 * blob and its two working copies small enough for the hub task's stack. */
#define LINK_MAX_DEVICE_NAMES 8

/* ---------------------------------------------------------------------------
 * Authentication (Link spec 6.2; bridge spec 8.9)
 * ------------------------------------------------------------------------- */

#define AUTH_PIN_DIGITS 6
#define AUTH_PIN_SALT_BYTES 16

/* The PIN's hash keeps it from being read straight out of a copy of the
 * flash. With only a million possible PINs no round count stops an offline
 * search, so the rate limits below are the real defence, and the count is
 * kept low: each check runs on the hub task, and 10,000 rounds took 1.5 s on
 * an ESP32-S3, holding up every session. 1,000 take about 150 ms. */
#define AUTH_PIN_PBKDF2_ITERATIONS 1000

#define AUTH_TOKEN_BYTES 32
#define AUTH_MAX_PAIRED_CLIENTS 16

/* last_seen is saved at most this often per client, to spare flash. */
#define AUTH_LAST_SEEN_SAVE_INTERVAL_MS (60 * 60 * 1000)

/* Rate limits for a fixed PIN (Link spec 6.2). */
#define AUTH_FAILURE_WINDOW_MS 60000
#define AUTH_MAX_FAILURES_PER_ADDRESS 5
#define AUTH_MAX_FAILURES_ALL_ADDRESSES 20
#define AUTH_LOCKOUT_MS 60000

/* Room for a paired client's name with its NUL; Console's names fit, and
 * longer ones are cut rather than refused. */
#define AUTH_CLIENT_NAME_BYTES 64

/* Addresses whose failures are counted at once. A home network has few
 * clients; one rotating through more addresses meets the all-addresses
 * limit instead. */
#define AUTH_TRACKED_ADDRESSES 8

/* ---------------------------------------------------------------------------
 * DSPi UART link (bridge spec 7; firmware control_interfaces_spec.md)
 * ------------------------------------------------------------------------- */

/* UART1 exists on every supported chip; UART0 stays with the boot console
 * (bridge spec 4.4). */
#define DSPI_UART_PORT_NUMBER 1

/* The recommended DSPi setting (bridge spec 5). */
#define DSPI_UART_DEFAULT_BAUD 1000000

/* Rates tried in order when detecting the baud rate, after the saved rate
 * (bridge spec 7.5). */
#define DSPI_UART_BAUD_CANDIDATES {1000000, 921600, 460800, 230400, 115200}

/* Firmware limits on the UART (Link spec 9.2): non-bulk GET replies are cut
 * to 132 bytes and non-bulk SET payloads above 64 bytes are refused. */
#define DSPI_UART_MAX_GET_REPLY_BYTES 132
#define DSPI_UART_MAX_SET_PAYLOAD_BYTES 64

/* Pause after every response so the firmware can send queued notifications
 * (Link spec 9.2, bridge spec 7.2). */
#define DSPI_UART_IDLE_GAP_MS 2

/* Added to twice the wire time of the largest possible reply. Covers the
 * DSPi's 45 ms flash-write blackout and its 50 ms internal busy retry
 * (bridge spec 7.2). */
#define DSPI_UART_TIMEOUT_MARGIN_MS 150

/* After a timeout, discard non-notification frames until the line has been
 * silent this long (bridge spec 7.4). */
#define DSPI_UART_QUARANTINE_SILENCE_MS 20

/* Upper bound on one quarantine, so a line that never falls silent (noise,
 * a wrong baud rate) cannot hold the link forever. */
#define DSPI_UART_QUARANTINE_MAX_MS 500

/* Attempts in a row with no valid answer (timeouts or corrupt answers)
 * before the UART driver is reinstalled. A receiver stuck on noise produces
 * corrupt frames, so they count too (bridge spec 7.4). Probes while looking
 * for a device do not: each changes the rate, which flushes the receiver. */
#define DSPI_UART_UNANSWERED_BEFORE_REINSTALL 3

/* Failed requests in a row before the device is reported lost. One timeout
 * never is (Link spec 8.2, bridge spec 7.6). */
#define DSPI_UART_FAILURES_BEFORE_LOST 3

/* How often to look for a device while none is known (bridge spec 7.6). */
#define DSPI_UART_PROBE_INTERVAL_MS 2000

/* Idle time after which a heartbeat request checks the device is still
 * there (bridge spec 7.6). */
#define DSPI_UART_HEARTBEAT_IDLE_MS 1000

/* While no request is waiting, the uart task checks for notifications this
 * often. Requests themselves start at once; this only bounds how late a
 * notification can be read. */
#define DSPI_UART_LISTEN_SLICE_MS 5

/* The ESP-IDF UART driver's receive ring. Holds over 40 ms of traffic at
 * 1 Mbaud, longer than the uart task ever goes without reading. */
#define DSPI_UART_DRIVER_RX_BUFFER_BYTES 4096

/* Requests waiting for the uart task: room for every session's in-flight
 * commands plus 8 for the hub's own polls and reads. */
#define DSPI_UART_QUEUE_DEPTH (LINK_MAX_SESSIONS_WITH_PSRAM * LINK_MAX_INFLIGHT + 8)

/* Bytes fetched from the UART driver in one read: enough to take a short
 * frame in one call without making the link state large. */
#define DSPI_UART_READ_CHUNK_BYTES 64

/* A frame whose bytes stop for this long is abandoned and its bytes after
 * the sync byte are searched again for a real frame: it began at a 0xA5
 * that was noise, or lost a byte. The DSPi sends a frame without pauses, so
 * a gap this long inside one never happens on a working line. */
#define DSPI_UART_FRAME_GAP_MS 20

/* When a wait for an answer runs out, bytes already received are still
 * parsed, up to what the driver's ring and the link's read-ahead hold. An
 * answer that arrived in time may have waited there while the uart task
 * was held up, by a handler or a busier task. */
#define DSPI_UART_LATE_PARSE_BYTES (DSPI_UART_DRIVER_RX_BUFFER_BYTES + DSPI_UART_READ_CHUNK_BYTES)

/* After a write returns, at most the 128-byte hardware FIFO is left to
 * send: 11 ms at 115200 baud. Anything longer means the UART is stuck. */
#define DSPI_UART_TX_DONE_TIMEOUT_MS 50

/* While idle, how long the uart task reads notifications after each listen
 * slice. Short, so a request queued meanwhile starts within a millisecond. */
#define DSPI_UART_IDLE_LISTEN_MS 1

/* How long dspi_uart_reconfigure() waits for room in a full queue. The
 * queue drains in a few hundred milliseconds even when every slot holds a
 * request; a caller that still finds no room gets ESP_ERR_TIMEOUT. */
#define DSPI_UART_RECONFIGURE_WAIT_MS 1000

/* ---------------------------------------------------------------------------
 * Tasks (bridge spec 6.2)
 * ------------------------------------------------------------------------- */

/* The uart task needs little stack: its buffers are static. */
#define DSPI_UART_TASK_STACK_BYTES 4096

/* Above the HTTP server and the hub, so a request in progress on the wire
 * is never held up by network work. */
#define DSPI_UART_TASK_PRIORITY 10

/* The uart task counts as alive if its loop has come round this recently.
 * One pass can include a search of every baud rate, a few seconds at most. */
#define DSPI_UART_ALIVE_WITHIN_MS 15000

/* ---------------------------------------------------------------------------
 * Networking (bridge spec 9)
 * ------------------------------------------------------------------------- */

/* A saved home network unreachable this long brings back the setup network
 * (bridge spec 9.1). */
#define WIFI_FALLBACK_AFTER_MS (3 * 60 * 1000)

/* Station retry delays while connecting; the last repeats (bridge spec 9.1). */
#define WIFI_RETRY_DELAYS_MS {5000, 10000, 20000, 30000}

/* hub.wifi.join gives the new network this long before returning to the old
 * one (Link spec 7.11). */
#define WIFI_JOIN_TIMEOUT_MS 30000

/* The setup network stays up after the home network connects until no phone
 * has been on it for this long (bridge spec 9.1). */
#define SETUP_NETWORK_IDLE_OFF_MS 60000

/* Delay between POST /setup/v1/finish and the setup network going off, so
 * the reply reaches the phone (bridge spec 9.3). */
#define SETUP_NETWORK_FINISH_DELAY_MS 5000

/* The setup network goes off by itself this long after a successful join
 * if the page never sends finish (bridge spec 9.3). */
#define SETUP_NETWORK_AUTO_OFF_MS (2 * 60 * 1000)

/* Phones or computers on the setup network at once (bridge spec 9.2). */
#define SETUP_NETWORK_MAX_STATIONS 4

/* The setup network's channel until the station joins, when it must follow
 * the station's. Channel 1 is allowed in every country. */
#define SETUP_NETWORK_CHANNEL 1

/* How often net_machine's timers run; the shortest of them is seconds. */
#define WIFI_TICK_MS 1000

/* A connection attempt with neither an address nor a failure after this
 * long is taken as lost and started again. A real one takes under 15 s. */
#define WIFI_ATTEMPT_TIMEOUT_MS 20000

/* A wrong password usually fails twice in a row; one failure can be a
 * handshake that merely timed out. */
#define WIFI_PASSWORD_FAILURES_BEFORE_WRONG 2

/* Scan results fetched from the driver: more networks than a home sees. */
#define WIFI_SCAN_MAX_RESULTS 32

/* A scan refused while the station is connecting is tried once more after
 * this long, by which time the attempt has usually finished. */
#define WIFI_SCAN_RETRY_DELAY_MS 1500

/* The setup page and hub.wifi.scan can ask at once; the driver runs one
 * scan at a time, so the second waits this long for the first, which takes
 * a few seconds, before it is answered busy. Short enough that the wait and
 * its own scan fit in the setup page's 8 s request timeout. */
#define WIFI_SCAN_LOCK_WAIT_MS 4000

/* A downgrade erases the newer firmware's extra keys from the `bridge`
 * namespace this many at a time, for at most this many rounds; any left
 * are never read and do no harm (bridge spec 11). */
#define SETTINGS_UNKNOWN_KEYS_PER_PASS 8
#define SETTINGS_UNKNOWN_KEY_PASSES 4

/* How long net_join() and net_finish_setup() wait for room in the event
 * queue, which the event task drains in milliseconds. */
#define NET_REQUEST_TIMEOUT_MS 1000

/* DNS-SD TXT entries one advertisement can carry, with room to spare over
 * those of Link spec 3.1. */
#define NET_MAX_TXT_ENTRIES 12

/* The setup network's DNS task: small, and below the HTTP servers (5). */
#define CAPTIVE_DNS_TASK_STACK_BYTES 3072
#define CAPTIVE_DNS_TASK_PRIORITY 4

/* The DNS task's wait for a question ends this often, so it never blocks
 * for ever; it simply waits again. */
#define CAPTIVE_DNS_RECEIVE_TIMEOUT_MS 1000

/* Pause after a socket error, so a broken socket cannot spin the task. */
#define CAPTIVE_DNS_ERROR_BACKOFF_MS 100

/* ---------------------------------------------------------------------------
 * HTTP servers and the setup API (bridge spec 8.2, 8.8, 9.3)
 * ------------------------------------------------------------------------- */

/* Sockets the Link server keeps beside its sessions, so pages and
 * /dspi/v1/info still load while every session is in use. */
#define HTTP_LINK_EXTRA_SOCKETS 3

/* Port 80's sockets. A phone's captive-portal browser opens several at once
 * and keeps them; the least recently used is dropped when all are taken. */
#define HTTP_SETUP_MAX_SOCKETS 7

/* Each server's task stack; the setup API builds its JSON replies there. */
#define HTTP_SERVER_STACK_BYTES 6144

/* Most bytes in one batch of a session's frames: one frame of the largest
 * size, so batching never splits the big frames into extra writes. */
#define HTTP_BATCH_MAX_BYTES LINK_MAX_FRAME_BYTES

/* Most bytes one run of the send pump writes before the server's task goes
 * back to reading what clients send. */
#define HTTP_PUMP_BYTES_PER_RUN 32768

/* A socket close ESP-IDF accepted but has not carried out is asked for
 * again after this long, so a lost request cannot keep a session open. */
#define HTTP_CLOSE_RETRY_MS 1000

/* The longest one HTTP request or WebSocket frame may take to arrive, or a
 * reply to leave, from its first byte. Each port has one task, so a client
 * that trickled a request a byte at a time would hold every other
 * connection up; past this budget it is dropped. The largest legitimate
 * transfers, the 23 KB control page or a 16 KB frame, take well under a
 * second even on a weak Wi-Fi link. */
#define HTTP_REQUEST_BUDGET_MS 8000

/* How long one read or write on a server socket may wait, in seconds. The
 * request budget above bounds the total. */
#define HTTP_SOCKET_WAIT_S 3

/* TCP keep-alive on the Link port: a connection idle this long is probed,
 * every interval, and dropped after this many probes go unanswered, so a
 * phone that left the network without closing frees its socket. */
#define HTTP_KEEP_ALIVE_IDLE_S 30
#define HTTP_KEEP_ALIVE_INTERVAL_S 5
#define HTTP_KEEP_ALIVE_COUNT 3

/* Most bytes of frames one session may have waiting to be sent, beside the
 * batch being written: room for LINK_MAX_INFLIGHT of the largest answers.
 * The frame count limit alone would let a client that reads slowly pin
 * hundreds of kilobytes of bulk answers. A close frame is always taken. */
#define HTTP_SESSION_QUEUE_MAX_BYTES 32768

/* Most messages, and bytes of them, one session may have handed to the hub
 * task and not yet seen handled. A client that sends faster than the hub
 * works is held back here, so it can neither fill the hub's queue for
 * everyone else nor pile up memory. A message that would go over waits
 * for room; one larger than the byte limit is let in once nothing else of
 * the session's is waiting. */
#define HTTP_SESSION_INBOUND_MAX_MESSAGES 8
#define HTTP_SESSION_INBOUND_MAX_BYTES 16384

/* How long a session's next message waits for that room. The hub handles a
 * message in well under this; a session still over its share after it is
 * closed with 1013 (try again later). */
#define HTTP_INBOUND_WAIT_MS 2000

/* A frame the bridge refuses (too large, or fragmented) is read and thrown
 * away up to this size, so the close frame that explains it reaches the
 * client; closing with the frame unread makes TCP reset the connection,
 * which loses the close frame. A larger one is cut off at once. */
#define HTTP_DISCARD_MAX_BYTES 65535

/* Largest JSON body the setup API reads: an apply request with the longest
 * network name, password and bridge name, and two PINs, fits in half. */
#define SETUP_API_MAX_BODY_BYTES 512

/* Networks the setup page lists, strongest first: more than a phone shows. */
#define SETUP_API_MAX_NETWORKS 24

/* ---------------------------------------------------------------------------
 * BOOT button (bridge spec 12.1)
 * ------------------------------------------------------------------------- */

#define BUTTON_FORGET_WIFI_HOLD_MS 5000
#define BUTTON_FACTORY_RESET_HOLD_MS 15000
#define BUTTON_CANCEL_HOLD_MS 30000

/* How often the pin is read, and how many readings in a row must agree
 * before a press or release counts: 60 ms, longer than contact bounce. */
#define BUTTON_SAMPLE_MS 20
#define BUTTON_DEBOUNCE_SAMPLES 3

/* ---------------------------------------------------------------------------
 * Status LED (bridge spec 13)
 * ------------------------------------------------------------------------- */

/* One full cycle of a fast blink (4 a second) and of a slow blink or pulse. */
#define LED_FAST_BLINK_MS 250
#define LED_SLOW_PERIOD_MS 2000

/* How long the LED shows green once the bridge is online with its DSPi. */
#define LED_READY_SHOW_MS 5000

/* How often the LED is redrawn: 50 times a second keeps a pulse smooth. */
#define LED_FRAME_MS 20

/* An addressable LED at full brightness is glaring; colours are scaled to
 * this out of 255. A plain LED is driven at full brightness. */
#define LED_WS2812_BRIGHTNESS 48

/* A plain LED's PWM frequency, far above any visible flicker. */
#define LED_PWM_FREQUENCY_HZ 5000

/* Longest status_led_configure() waits for a frame in progress, which takes
 * well under a millisecond. */
#define LED_CONFIGURE_TIMEOUT_MS 100

/* ---------------------------------------------------------------------------
 * Firmware updates (bridge spec 14)
 * ------------------------------------------------------------------------- */

/* A new image is first checked after running this long; a crash or restart
 * before it is confirmed rolls back (bridge spec 14.2). */
#define OTA_CONFIRM_AFTER_MS 60000

/* A new image that is not healthy yet (Wi-Fi still joining, say), or whose
 * confirmation could not be written, is checked again this often. */
#define OTA_CONFIRM_RETRY_MS 10000

/* A new image still not healthy this long after starting is given up and
 * the previous one restored. Longer than WIFI_FALLBACK_AFTER_MS, so a home
 * network that is down gives way to the setup network before then. */
#define OTA_CONFIRM_GIVE_UP_MS (5 * 60 * 1000)

/* ---------------------------------------------------------------------------
 * Boot and supervision (bridge spec 6.5 and 12.3)
 * ------------------------------------------------------------------------- */

/* A boot step that fails waits this long, with the BOOT button watched,
 * before restarting: a restart loop otherwise comes round faster than
 * anyone could hold BOOT for a factory reset. */
#define BOOT_FAILURE_RESTART_MS 60000

/* How often the supervisor checks that the hub task and the uart task are
 * still going round their loops, and how long either may stay stopped
 * before it restarts the bridge. */
#define SUPERVISOR_CHECK_MS 5000
#define SUPERVISOR_RESTART_AFTER_MS 30000

/* An update with no chunk written or received for this long is abandoned
 * (bridge spec 14.2). Generous: a phone that locks its screen pauses the
 * page for a while. */
#define OTA_IDLE_TIMEOUT_MS 30000

/* Chunks waiting to be written to flash. Each holds up to one 16 KB frame,
 * so this bounds the memory an update takes; the client keeps no more than
 * this many chunks unanswered (bridge spec 14.2). */
#define OTA_QUEUE_DEPTH 2

/* Writing flash is slow, so the ota task runs below the hub task, which
 * keeps answering while an image goes in. */
#define OTA_TASK_PRIORITY 3
#define OTA_TASK_STACK_BYTES 4096

/* How long ota_abort() waits for room in the ota task's queue. A chunk takes
 * tens of milliseconds to write, so room comes quickly. */
#define OTA_ABORT_TIMEOUT_MS 100

/* ---------------------------------------------------------------------------
 * The hub and the hub task (bridge spec 6.2 and 8)
 * ------------------------------------------------------------------------- */

/* How often hub_tick() runs: a fifth of the fastest poll period (50 ms at
 * LINK_POLL_MAX_HZ), so polls go out close to their schedule. */
#define HUB_TICK_MS 10

/* Distinct polls across all sessions; identical polls share one. The poll
 * budget runs out long before this many different polls are useful. */
#define HUB_MAX_POLL_GROUPS 32

/* Recent SETs remembered for origin attribution (Link spec 8.3). When full,
 * the oldest, the least likely to match, is forgotten. */
#define HUB_ORIGIN_QUEUE_ENTRIES 16

/* With the PIN on, sessions from one address that have not yet
 * authenticated; a further one closes the oldest. Two lets a client
 * reconnect while its last attempt is still timing out, yet one stranger
 * cannot fill every slot with sessions that never pair. */
#define HUB_UNAUTHENTICATED_PER_ADDRESS 2

/* How often a connected paired client is marked seen, so the client a full
 * list forgets is one that is away, not one that stays connected. Saved
 * to flash at most every AUTH_LAST_SEEN_SAVE_INTERVAL_MS. */
#define HUB_AUTH_TOUCH_INTERVAL_MS 60000

/* After poll.error a failing poll is retried this often until it answers
 * again; Link spec 8.4 asks only for "a reduced rate". */
#define HUB_POLL_FAILED_PERIOD_MS 1000

/* While a firmware update is written, flash erases pause the uart task for
 * up to about 45 ms each, and the UART's receive buffer must hold whatever
 * arrives meanwhile. So a client GET that could bring back more than half
 * of it is answered BUSY until the update ends, and polls and snapshot
 * reads wait (bridge spec 14.2). */
#define HUB_UPDATE_MAX_GET_BYTES (DSPI_UART_DRIVER_RX_BUFFER_BYTES / 2)

/* hub.stats' avg_rtt_ms is a moving average in which each new round trip
 * counts 1/HUB_RTT_AVERAGE_WEIGHT: smooth, yet it follows a change within a
 * few dozen commands. */
#define HUB_RTT_AVERAGE_WEIGHT 8

/* A request whose answer has not come this long after it became the oldest
 * is taken as lost on the way and finishes as a timeout. Longer than the
 * uart task's longest pass (DSPI_UART_ALIVE_WITHIN_MS), so a slow answer
 * is never given up. */
#define HUB_REPLY_LOST_AFTER_MS (DSPI_UART_ALIVE_WITHIN_MS + 5000)

/* The hub task's stack holds one event and the hub's JSON work; the hub
 * itself is static. It runs below the uart task. */
#define HUB_TASK_STACK_BYTES 6144
#define HUB_TASK_PRIORITY 5

/* Events waiting for the hub task. The last HUB_EVENT_LIFECYCLE_RESERVE
 * places are kept for events that must not be lost (sessions opening and
 * closing, the device coming and going, update progress, a restart): two
 * per session, two for the device, OTA_QUEUE_DEPTH + 1 for an update, one
 * restart, and one for each of the six tasks that post, which can race for
 * the last ordinary place. */
#define HUB_EVENT_QUEUE_DEPTH 64
#define HUB_EVENT_LIFECYCLE_RESERVE                                                                \
    (2 * LINK_MAX_SESSIONS_WITH_PSRAM + 2 + OTA_QUEUE_DEPTH + 1 + 1 + 6)

/* How long a task waits for room in the hub task's queue before it drops
 * an ordinary event (a session's message) and counts it. A lifecycle event
 * waits longer, since losing one leaves a slot or the device stuck. The
 * uart task waits less (HUB_UART_LIFECYCLE_POST_WAIT_MS). */
#define HUB_EVENT_POST_TIMEOUT_MS 1000
#define HUB_LIFECYCLE_POST_TIMEOUT_MS 5000

/* The uart task never waits on the hub task for long: a request on the
 * wire must not be held up. Answers and notifications are posted without
 * waiting (a dropped answer is finished as lost by the hub; a dropped
 * notification makes the clients re-read), and the device coming or going
 * waits at most this long for a place in the reserve; if even that fails,
 * the hub task picks the change up on its next tick. */
#define HUB_UART_LIFECYCLE_POST_WAIT_MS 10

/* Dropped events and unsent messages are counted, and the counts logged at
 * most this often: a log line per drop, on the 115200-baud console, would
 * slow the task that drops to a crawl just when it is busiest. */
#define HUB_DROP_LOG_INTERVAL_MS 5000

/* When new settings put the status LED on a pin the UART is leaving, the
 * hub task waits up to this long for the uart task to have moved before it
 * starts the LED there. The move waits behind at most a queue of requests,
 * a few hundred milliseconds; this stays well inside HUB_ALIVE_WITHIN_MS. */
#define HUB_UART_MOVE_WAIT_MS 1000

/* Most another task waits to queue a call on the hub task (hub_task_*()). */
#define HUB_CALL_TIMEOUT_MS 5000

/* Most a task waits for the lock on the info document and the hub's name,
 * which is held only for a copy. */
#define HUB_INFO_LOCK_TIMEOUT_MS 1000

/* Buffers carrying answers and notifications from the uart task to the hub
 * task, so that path never allocates. Small ones take every notification
 * and non-bulk answer; large ones the bulk reads, which the UART carries
 * one at a time. An answer that finds none free arrives as a timeout, a
 * notification is dropped; both are counted (HUB_DROP_LOG_INTERVAL_MS). */
#define HUB_EVENT_SMALL_BUFFERS 32
#define HUB_EVENT_SMALL_BUFFER_BYTES DSPI_UART_MAX_GET_REPLY_BYTES
#define HUB_EVENT_LARGE_BUFFERS 2
#define HUB_EVENT_LARGE_BUFFER_BYTES LINK_MAX_PAYLOAD_BYTES

/* A restart waits this long so the replies and close frames go out first. */
#define HUB_RESTART_DELAY_MS 500

/* The hub task counts as alive if its loop has run this recently. It runs
 * every HUB_TICK_MS; this allows for a long burst of work such as a PIN
 * hash (bridge spec 14.2). */
#define HUB_ALIVE_WITHIN_MS 2000

/* hub.wifi.scan runs in its own short-lived task, one scan at a time, so
 * the hub task never waits seconds for it. The list of networks found is
 * cut to WIFI_SCAN_MAX_NETWORKS, more than a page shows usefully. */
#define WIFI_SCAN_TASK_STACK_BYTES 4096
#define WIFI_SCAN_TASK_PRIORITY 2
#define WIFI_SCAN_MAX_NETWORKS 24

/* Clock readings before this (2024-01-01) mean SNTP has not set the time
 * yet, so paired clients' times are left out (bridge spec 8.9). */
#define CLOCK_SET_AFTER_UNIX_S 1704067200

#endif /* BRIDGE_CONFIG_H */
