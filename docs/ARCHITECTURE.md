# HIDway: απομακρυσμένο φυσικό keyboard/mouse μέσω πραγματικού USB HID

## Context

Το AION 2 (anti-cheat NCGuard/VIOLET, έντονη πολιτική κατά των bots) μπλοκάρει το input που έρχεται από software, δηλαδή injected input μέσω SendInput (LLMHF_INJECTED). Γι' αυτό δεν δουλεύει μέσα στο game το input από Steam Remote Play ή AnyDesk.

Ο στόχος είναι μια συσκευή τύπου hardware KVM:
- Το gaming PC βλέπει μόνο ένα κανονικό USB HID keyboard και mouse.
- Το input ξεκινά από το remote PC και φτάνει μέσω Internet.
- Video και audio μένουν στο Steam Remote Play.
- Δεν υπάρχει spoofing ούτε απόκρυψη. Η συσκευή δηλώνει τίμια τι είναι.
- Δεν υπάρχουν macros ή automation. Είναι αυστηρά relay 1:1 του ανθρώπινου input.

**Δεδομένα:**
- Raspberry Pi 4. Μένει server στο LAN με τις υπάρχουσες υπηρεσίες του και το Ethernet του είναι ήδη πιασμένο. Βρίσκεται κοντά στο gaming PC.
- Pico W.
- Tailscale ήδη στημένο.
- Όνομα: **HIDway**.
- Repo: `C:\Projects\RemoteControl`, που είναι κενό.

## Ετυμηγορία

Η αρχική ιδέα (Pi ως gateway και Pico ως HID) **είναι σωστή στον πυρήνα της**. Την κρατάω με 5 αλλαγές:

1. **Transport μέσω Tailscale** αντί για χειροκίνητο WireGuard. Είναι WireGuard από κάτω και το έχεις ήδη. Πρέπει όμως να επιβεβαιώσουμε ότι η σύνδεση είναι **direct** και όχι μέσω DERP relay.
2. **Pi → Pico μέσω USB**, με ένα Raspberry Pi Debug Probe (CDC-UART + SWD), αντί για GPIO UART.
   - Δεν χρειάζεται αλλαγή στο config.txt ούτε reboot του Pi, που τρέχει άλλες υπηρεσίες.
   - Δίνει stable device path.
   - Επιτρέπει **remote flashing και reset του Pico μέσω SWD**.
3. **Κανόνας «latest state wins, no queues»** σε κάθε hop. Το Pi είναι thin relay και όλη η λογική βρίσκεται στο client και στο Pico, που έχει δικό του, ανεξάρτητο failsafe.
4. **Phase 0 go/no-go**. Πριν γραφτεί οτιδήποτε άλλο, αποδεικνύουμε ότι το AION 2 δέχεται input από Pico HID. Αν δεν το δέχεται, σταματάμε.
5. **Ένα κοινό C header** για το wire protocol. Τον μοιράζονται το firmware, ο relay και το client, και όλα γράφονται σε C.

## Τελική αρχιτεκτονική

```
REMOTE PC (Windows)                          ΣΠΙΤΙ
┌───────────────────────────┐              ┌──────────── Pi 4 (υπάρχων server) ────────────┐
│ Steam client  ◄── video ──┼──────────────┤ (Steam stream από gaming PC)                   │
│ hidway-client.exe (C)     │  UDP μέσα σε │ tailscaled ──► hidwayd (C, SCHED_FIFO)         │
│  WH_KEYBOARD_LL (capture) │  Tailscale   │   bind μόνο στο 100.x:47800, allowlist client  │
│  Raw Input mouse (deltas) ├─────────────►│   drop stale seq → COBS+CRC16 → serial         │
│  sender ≤1 kHz / 50 Hz hb │◄── status ───┤   status/RTT echo κάθε 50 ms                   │
└───────────────────────────┘              └───────────────┬────────────────────────────────┘
                                                           │ USB
                                            Raspberry Pi Debug Probe (CDC-UART 921600 + SWD)
                                                           │ UART GP0/GP1/GND (1 kΩ σε σειρά) + SWD
                                            Pico W: TinyUSB, ITF0 boot keyboard, ITF1 mouse
                                                           │ USB Full-Speed, bInterval=1 (1 kHz)
                                            GAMING PC: κανένα νέο software, κανένας driver
```

## Αξιολόγηση εναλλακτικών

| Επιλογή | Απόφαση | Λόγος |
|---|---|---|
| **Pi + Pico (επιλεγμένη)** | ✅ | Το MCU είναι deterministic, κάνει enumerate σε λιγότερο από 1 s, έχει **ανεξάρτητο watchdog** και ζει ξεχωριστά από το Linux. Το Pi μπορεί να κάνει reboot ή update χωρίς stuck keys. |
| Pi 4 απευθείας ως USB gadget (dwc2/configfs) | Απορρίπτεται για σένα | Τεχνικά δουλεύει (το PiKVM κάνει ακριβώς αυτό). Όμως το USB-C είναι **και η τροφοδοσία** του Pi, άρα χρειάζεται splitter. Δένει επίσης τη ζωή ενός server με το gaming PC. Και αν πέσει το daemon, το `hidg` κρατά την τελευταία κατάσταση, άρα υπάρχει ρίσκο stuck key χωρίς ανεξάρτητο failsafe. |
| Pico W απευθείας από Internet | Απορρίπτεται | Δεν υπάρχει Tailscale για MCU. Θα εκθέταμε MCU στο Internet. Έχει jitter από 2.4 GHz και έχουν αναφερθεί UDP stalls στο cyw43. |
| Pico W μέσω Wi-Fi από το Pi στο LAN | Μόνο ως fallback | Δεν θέλει καλώδια, αλλά έχει Wi-Fi jitter. Χρειάζεται μέτρηση και δεν το προτείνω ως κύριο. |
| USB-over-IP (VirtualHere, usbip) | Απορρίπτεται | Απαιτεί **virtual USB host controller driver στο gaming PC**, που παραβιάζει το requirement. Το latency σε WAN είναι κακό. |
| Hardware USB extender (Cat5/fiber) | Απορρίπτεται | Δουλεύει μόνο σε απόσταση LAN, όχι μέσω Internet. |
| KVM-over-IP (PiKVM, JetKVM, GL.iNet Comet) | Βιώσιμο αν θες να «αγοράσεις» | Δίνει πραγματικό HID, αλλά με input μέσω browser και WebSocket (TCP), σχεδιασμένο για admin χρήση. Η υποστήριξη relative mouse διαφέρει ανά συσκευή 🟡, και πληρώνεις video capture που δεν χρειάζεσαι. |
| Το gaming PC ως network endpoint, με USB προς το Pico | Απορρίπτεται | Βάζει software του gaming PC στο input path. Σπάει σε login screen και BIOS. Είναι επίσης το ίδιο μοτίβο με το cheat hardware (HID loopback ελεγχόμενο από το ίδιο PC). |
| ESP32-S3 / Teensy 4 (USB HS, 8 kHz) | Δεν χρειάζεται | Το 1 kHz Full-Speed αρκεί και το Pico W το έχεις ήδη. |
| TCP / QUIC / WebRTC | Απορρίπτεται | Το state replication κάνει περιττή την αξιοπιστία μεταφοράς. Το TCP έχει head-of-line blocking. Το UDP είναι το σωστό. |

**Γιατί χρειάζεται το Pi:** Το Tailscale θέλει πλήρες OS, και το Pi προσθέτει μόνο περίπου 0.1–0.5 ms. Δεν είναι άσκοπο latency.

## Software ανά μηχάνημα

| Μηχάνημα | Τι τρέχει |
|---|---|
| **Gaming PC** | **Τίποτα νέο.** Το Windows HID class driver είναι ενσωματωμένο. Το Steam (ή το Sunshine) μένει για το video. |
| **Pi 4** | Tailscale (υπάρχει). `hidwayd` (C, systemd, user `hidway`, SCHED_FIFO). udev rule για το probe. Προαιρετικά `openocd` (SWD flashing) και `wakeonlan`. |
| **Remote PC** | Tailscale, Steam client, `hidway-client.exe` (C/Win32, tray icon) και `hidway.ini`. |
| **Pico W** | HIDway firmware: Pico SDK 2.x με το ενσωματωμένο TinyUSB, `PICO_BOARD=pico_w`. |
| **Dev PC** | Pico SDK toolchain (arm-none-eabi-gcc, CMake, Ninja), MSVC Build Tools ή MinGW-w64, Python 3 για τα tools. |

**Hardware BOM:**
- Pico W, το έχεις ήδη.
- Raspberry Pi Debug Probe, περίπου €12. Εναλλακτικά ένα δεύτερο Pico με το debugprobe firmware, ή ένα CP2102N/FT232RL 3.3V (χωρίς SWD).
- Καλό micro-USB data καλώδιο προς **πίσω** θύρα του motherboard, όχι σε hub ή front panel.
- 2 αντιστάσεις 1 kΩ σε σειρά στα TX/RX. Περιορίζουν το back-powering όταν το Pico είναι unpowered.
- Προαιρετικά: διακόπτης ARM στο GP14, LED στο GP15 και 3 pins SWD.

## Protocol και data flow

**Αρχή σχεδιασμού:** κάθε πακέτο μεταφέρει την **πλήρη κατάσταση**. Δεν στέλνουμε events.
- Χαμένο πακέτο: το επόμενο το διορθώνει.
- Stuck key από χαμένο key-up: αδύνατο.
- Autorepeat: γίνεται dedupe φυσικά, και το typematic το κάνει το gaming PC.

**Client → Pi** (UDP, little-endian, περίπου 51 bytes):

```
u8 magic 'H' | u8 ver=1 | u8 type (STATE/RELEASE/CMD) | u8 flags
u32 session_id (random ανά εκκίνηση client) | u32 seq (αυστηρά αύξων) | u32 client_time_us
u8 mods (E0–E7) | u8 keys[21] (bitmap HID usages 0x00–0xA7) | u8 buttons (L,R,M,X1,X2)
i32 mouse_x_cum | i32 mouse_y_cum | i16 wheel_v_cum | i16 wheel_h_cum
```

**Pi → Pico** (UART frame, περίπου 41 bytes): `COBS( type, seq16, mods, keys[21], buttons, x_cum, y_cum, wv, wh, CRC16-CCITT ) 0x00`.
- Στα 921600 baud, ένα frame παίρνει περίπου 0.45 ms. Στο 1 kHz η χρήση της γραμμής είναι περίπου 45%.
- **Το UART δεν είναι bottleneck** ✅ (αριθμητικά).

**Pico → Pi → Client (status κάθε 100 ms):**
- usb_mounted, armed, boot/report protocol, κατάσταση Caps/Num LED του gaming PC.
- Counters: frames_ok, crc_err, link_timeouts, watchdog_resets.
- Echo του `client_time_us` για RTT.

**Κανόνες ανά hop:**
- **Client:**
  - Ο keyboard hook αποθηκεύει HID usage από scan code (με χάρτη από το Microsoft scan-code spec), όχι VK. Έτσι το layout (ελληνικά/αγγλικά) εφαρμόζεται στο gaming PC, όπως σε πραγματικό keyboard.
  - Το mouse διαβάζεται από Raw Input: raw counts 1:1, χωρίς acceleration.
  - Στέλνει αμέσως σε κάθε αλλαγή, coalesced σε ≤1 kHz, και heartbeat 50 Hz όταν δεν γίνεται τίποτα.
  - Σε κάθε αλλαγή key ή button στέλνει **2 επιπλέον αντίγραφα** (+1 ms, +3 ms), για απώλεια σε edges.
- **Pi:**
  - Δέχεται μόνο από το allowlisted Tailscale IP.
  - Πετάει `seq ≤ last` (reorder ή duplicate). Με cumulative counters αυτό δεν χάνει τίποτα.
  - Νέο `session_id` σημαίνει reset.
  - **Δεν βάζει ποτέ ουρά:** αν το tty έχει ήδη ένα frame σε αναμονή (`TIOCOUTQ`), κρατά μόνο το πιο πρόσφατο.
  - Δεν ξαναστέλνει ποτέ stale client state.
- **Pico:**
  - `delta = x_cum − last_x_cum` με int32 wrap-safe αφαίρεση.
  - **Catch-up policy:** αν το κενό από το προηγούμενο valid frame είναι ≤100 ms, εφαρμόζεται όλο το delta (δεν χάνεται κίνηση). Αν είναι μεγαλύτερο, γίνεται resync και το delta απορρίπτεται, ώστε να μη γίνει «τίναγμα» κάμερας μετά από outage. Το όριο είναι configurable.
  - Το πρώτο frame κάθε session ορίζει τη baseline.
  - Δεν υπάρχει jitter buffer (θα πρόσθετε latency). Τα clumped πακέτα απλώς αθροίζονται στο επόμενο USB report.
- **USB descriptors** (Phase 1):
  - **ITF0:** boot keyboard 6KRO, για BIOS.
  - **ITF1:** mouse που υποστηρίζει boot protocol. Σε report protocol στέλνει **16-bit X/Y** (απαραίτητο: γρήγορο flick σε 1600 DPI ξεπερνά τα ±127 counts/ms), 5 buttons (το 4/5 γίνεται XBUTTON1/2 στα Windows), wheel και AC Pan. Σε boot protocol στέλνει το κλασικό 3-byte report.
  - bInterval=1.
  - Ταυτότητα: VID 0x2E8A (Raspberry Pi) με PID από το `raspberrypi/usb-pid`, strings «HIDway». **Δεν μιμείται άλλον vendor.**
- **NKRO** (Phase 3): ξεχωριστό ITF2 bitmap keyboard, σε στυλ QMK. Αν ο host δεν κάνει poll το ITF2 (BIOS), γίνεται fallback σε ITF0. Το hi-res wheel (Resolution Multiplier) είναι προαιρετικό.

## Failure και recovery

| Αστοχία | Ανίχνευση | Ενέργεια | Όριο |
|---|---|---|---|
| Client crash/kill | Το Pi δεν λαμβάνει πακέτο για 150 ms. Το Pico δεν λαμβάνει frame για 250 ms. | Το Pi στέλνει RELEASE. Το Pico κάνει release all ανεξάρτητα. | ≤250 ms |
| Remote PC κάνει lock/sleep | `WTSRegisterSessionNotification` | 3 RELEASE και τέλος capture | άμεσα |
| Πέφτει το Internet ή το Tailscale | Timeout στο Pi και στο Pico. Το client δεν λαμβάνει status για 500 ms. | Release στο gaming PC. Το tray γίνεται κόκκινο. Το capture σταματά αυτόματα μετά από 2 s, ώστε να μη μείνεις «κλειδωμένος». | ≤250 ms |
| Spike μικρότερο από το timeout | — | Η κατάσταση συνεχίζει και η κίνηση ακολουθεί το catch-up policy | — |
| Crash του hidwayd | Timeout στο Pico. Restart από systemd (Restart=always, WatchdogSec). | Release και recovery σε περίπου 1 s | ≤250 ms |
| Reboot ή διακοπή ρεύματος στο Pi, αποσύνδεση του probe | Timeout στο Pico. Το daemon ξανανοίγει το `/dev/serial/by-id` με retry. | Release. Το HID **μένει enumerated**. | ≤250 ms |
| Αλλοίωση δεδομένων στο UART | CRC fail | Το frame απορρίπτεται και το επόμενο (≤1 ms) αποκαθιστά την κατάσταση | — |
| Κόλλημα του Pico firmware | HW watchdog 100 ms | Reset και re-enumerate. Τα Windows αφαιρούν τη συσκευή, άρα όλα γίνονται release. | περίπου 1 s |
| Emergency, remote | Hotkey στο client | RELEASE και **soft-detach** (`tud_disconnect`): η συσκευή εξαφανίζεται από τα Windows | άμεσα |
| Emergency, local | Διακόπτης ARM ή αποσύνδεση | Release και αγνόηση του UART | άμεσα |

**Ειδικά για το client:**
- Ο LL hook πρέπει να επιστρέφει σε λίγα μs. Τρέχει σε ξεχωριστό thread με δικό του message loop και γράφει σε lock-free state. Τα Windows 7 και νεότερα **αφαιρούν σιωπηλά** hook που περνά το `LowLevelHooksTimeout` ✅ (το πρόβλημα αυτό αποκλείει Python ή GC runtimes για το client).
- Το Ctrl+Alt+Del και το Win+L **δεν** μπορούν να μπλοκαριστούν τοπικά ✅. Γι' αυτό το Ctrl+Alt+End στέλνει Ctrl+Alt+Del στο gaming PC, σε στυλ RDP.
- Hotkeys (configurable): Ctrl+Shift+F12 για toggle του capture, και ένα emergency chord για release και detach.

## Security

1. **Δίκτυο:**
   - Tailscale: WireGuard με Noise IK, ChaCha20-Poly1305 και anti-replay counter window ✅.
   - Το cryptokey routing εγγυάται ότι η source IP ανήκει στον authenticated peer.
   - **Tailscale ACL:** μόνο `remote-pc → pi:47800/udp`.
   - Το `hidwayd` κάνει bind **μόνο στο Tailscale IP** και επιπλέον ελέγχει την source IP. Δεν ανοίγει καμία πόρτα στο router, και ο HID endpoint δεν εκτίθεται πουθενά.
2. **Pi (shared server):**
   - Dedicated user.
   - udev rule: probe και tty με `0660 group hidway`.
   - systemd hardening: `NoNewPrivileges`, `ProtectSystem=strict`, `PrivateTmp`.
   - Rate limit στα 2000 pps/IP και sanity cap στα deltas.
   - Το πραγματικό ρίσκο: **όποιος αποκτήσει root στο Pi μπορεί να «πληκτρολογεί» στο gaming PC.** Αυτό ισχύει by design για κάθε KVM. Γι' αυτό το Pi θέλει SSH μόνο με keys και updates.
3. **Pico:**
   - Rate cap ανά report.
   - Ο διακόπτης ARM δίνει φυσική απενεργοποίηση όταν είσαι σπίτι.
4. **Προαιρετικό (Phase 4), end-to-end MAC client → Pico:** SipHash με nonce που εκδίδει το Pico. Προστατεύει από replay ενός session στο login screen από ένα compromised Pi. Έχει ένα trade-off: όσο το SWD είναι συνδεδεμένο, ο root του Pi μπορεί να κάνει reflash. Το MAC έχει νόημα μόνο αν το SWD αποσυνδέεται εκτός maintenance.
5. **Αν χαθεί το VPN ή το client**, η απάντηση είναι πάντα fail-safe: release all και disarm. Δεν υπάρχει κατάσταση όπου χάνεται η σύνδεση και τα πλήκτρα μένουν πατημένα.

## Latency (one-way, input path)

| Στάδιο | Τυπικά | p99 |
|---|---|---|
| Raw Input → client, coalescing 1 kHz | 0.5–1 ms | 2 |
| Tailscale encrypt/decrypt (στις 2 άκρες) | 0.2–0.6 | 2 |
| Remote LAN: Ethernet / Wi-Fi | 0.3 / 2 | 1 / 10+ |
| **Internet, ρεαλιστικό σενάριο Ελλάδας (RTT 15–30 ms)** | **8–15** | **+5–20 spikes** |
| Router → Pi, hidwayd (SCHED_FIFO) | 0.3 | 1 |
| Debug Probe CDC→UART + μετάδοση 41 B | 0.5–1.5 | 2–3 🧪 |
| Pico + USB poll 1 kHz + Windows HID stack | 0.7–1.5 | 2 |
| **Σύνολο στο LAN** | **≈3–6 ms** | **≈10 ms** |
| **Σύνολο μέσω Internet (direct Tailscale)** | **≈12–25 ms** | **≈30–50 ms** |

- Το input path έχει **περίπου το ίδιο κόστος** με το input του ίδιου του Steam: διασχίζει το ίδιο Internet μία φορά.
- Glass-to-glass, με το Steam video (encode, network, decode, display), η συνολική εμπειρία είναι περίπου **45–80 ms**, τυπικό για remote gaming.
- Το jitter το καθορίζει το Internet και το Wi-Fi του remote PC. **Σύσταση: remote PC σε Ethernet.**
- Αν το Tailscale πάει μέσω **DERP**, προστίθενται 10–50 ms και bursts, γιατί το relay είναι TCP/HTTPS. Πρέπει να το ελέγξουμε με `tailscale ping`.
- Bandwidth: περίπου 1.1 Mbps upload όταν κινείται το mouse, με ρύθμιση για 500 Hz.

## Τι είναι αποδεδειγμένο και τι όχι

✅ **Επιβεβαιωμένο:**
- Το RP2040 είναι USB 1.1 Full-Speed device.
- Το TinyUSB κάνει composite boot keyboard και mouse σε ξεχωριστά interfaces (επίσημο example `hid_boot_interface`).
- Το FS interrupt endpoint έχει ελάχιστο interval 1 ms, άρα μέγιστο 1000 Hz.
- Δεν χρειάζεται driver στα Windows.
- Το boot protocol είναι αυτό που χρησιμοποιεί το BIOS.
- Τα buttons 4/5 γίνονται XBUTTON1/2 στα Windows.
- Το UART έχει αρκετό bandwidth (αριθμητικά).
- Το WireGuard και το Tailscale δίνουν encryption, auth και anti-replay.
- Ο LL hook μπορεί να αφαιρεθεί σιωπηλά από timeout. Το Ctrl+Alt+Del δεν μπλοκάρεται.
- Το Sunshine έχει επιλογές `keyboard = disabled` και `mouse = disabled`.
- Το Pi 4 υποστηρίζει OTG στο USB-C (απορρίφθηκε για άλλους λόγους).
- Η NCSOFT εφαρμόζει «hardware-based restrictions» και permanent bans κατά των bots.

🟢 **Πολύ πιθανό:**
- Η είσοδος HID δουλεύει σε login screen, UAC και secure desktop, γιατί είναι κανονικό keyboard.
- Το NKRO bitmap δουλεύει στα Windows (όπως QMK και gaming keyboards).
- Το 16-bit mouse σε report protocol λειτουργεί με το mouhid.
- Η απόδοση του Pi δεν επηρεάζεται από τις υπόλοιπες υπηρεσίες του (το hidwayd είναι SCHED_FIFO με αμελητέο CPU).

🟡 **Αβέβαιο:**
- Αν **μελλοντικό update** του anti-cheat θα μπλοκάρει ή θα κάνει flag μη-mainstream HID VIDs. Δεν το ελέγχει κανείς από εμάς.
- Η σχετική λειτουργία mouse στα εμπορικά KVM-over-IP.
- Το hi-res wheel.

🧪 **Απαιτεί πραγματική δοκιμή, κανείς δεν μπορεί να το εγγυηθεί:**
- **(T0) αν το AION 2 δέχεται το Pico HID.**
- Αν το Tailscale βγάζει direct path στα δικά σου NAT.
- Το πραγματικό latency του Debug Probe UART.
- Αν, όταν ο WH_MOUSE_LL κάνει block, φτάνει ακόμα το WM_INPUT στο client στα Windows 11 (fallback: ClipCursor και hidden cursor πάνω σε δικό μας παράθυρο).
- Διπλό input στο desktop από το Steam client.
- Το BIOS και UEFI του δικού σου motherboard.
- Wake από S3/S5 (εναλλακτικά, WoL από το Pi).
- Σταθερότητα σε soak 8 ωρών.

## Ρίσκο λογαριασμού

Σε επίπεδο Windows, η συσκευή είναι ένα κανονικό keyboard. Το ότι λειτουργούν τα onboard macros του Razer είναι συμβατό με αυτό: το firmware της συσκευής στέλνει πραγματικά HID reports.

Λειτουργικά όμως είναι ένα **HID ελεγχόμενο από δίκτυο**, δηλαδή η ίδια κατηγορία συσκευών που χρησιμοποιούν και οι botters. Η NCSOFT ήδη κυνηγά τα bots, ακόμα και με απότομες κινήσεις, όπως το block του Synapse και του G Hub τον Δεκέμβριο. Το HIDway δεν έχει macros ή automation και δεν κρύβει την ταυτότητά του, όμως το anti-cheat δεν μπορεί να ξέρει την πρόθεση.

**Πρόταση:**
- Διάβασε τα ToS του AION 2 ή ρώτα το support για KVM-over-IP.
- Δέξου ότι ένα μελλοντικό update μπορεί να σταματήσει τη λειτουργία της συσκευής.
- Δεν θα προστεθεί ποτέ μηχανισμός απόκρυψης.

## Repo layout (`C:\Projects\RemoteControl`)

```
common/    protocol.h (wire format, μοναδική πηγή), cobs.[ch], crc16.[ch], keymap_sc2hid.h, catchup.[ch]
firmware/  CMakeLists.txt (pico-sdk), src/{main,usb_descriptors,link,hid_out,status}.c, tusb_config.h
relay/     CMakeLists.txt, src/hidwayd.c, hidwayd.service, 99-hidway.rules, hidwayd.conf.example
client/    CMakeLists.txt, src/{main,capture_kbd,capture_mouse,net,tray,config,bench}.c, hidway.ini.example
tests/     host unit tests για common/ (ctest): COBS, CRC, keymap, wrap-safe catch-up
tools/     fake_client.py (soak/fuzz), frame_dump.py, flash_pico.sh (openocd μέσω SWD από το Pi)
docs/      ARCHITECTURE.md (αυτή η ανάλυση), PROTOCOL.md, TESTING.md, ROADMAP.md
```

Commits χωρίς Co-Authored-By trailer, όπως λέει το global CLAUDE.md σου. Το `/vault` χρειάζεται μόνο αν μπει το MAC της Phase 4.

## Implementation plan

**Phase 0: Go/No-Go (1–2 ώρες, δεν χρειάζεται probe)**
- Bootstrap: `git init`, `.gitignore`, skeleton, `CLAUDE.md`.
- `firmware/` με τους **τελικούς** HIDway descriptors (ITF0 boot keyboard, ITF1 16-bit mouse).
- Mode `T0`: όσο είναι πατημένο το BOOTSEL, κρατά πατημένο το «W» και στέλνει mouse X +5 counts/ms.
- **Exit:** περνά ή αποτυγχάνει το T0. Αν αποτύχει, σταματάμε και καταγράφουμε το αποτέλεσμα.
- Παραγγελία Debug Probe παράλληλα.

**Phase 1: LAN prototype**
- `common/` με unit tests.
- Firmware v0.1: UART RX (IRQ σε ring buffer), COBS/CRC, εφαρμογή state, catch-up, link timeout 250 ms, HW watchdog, LED.
- `hidwayd` v0.1: UDP → serial, latest-only, systemd unit, udev rule.
- Client v0.1:
  - Thread για LL keyboard hook (capture και swallow).
  - Thread για Raw Input mouse (message-only window, INPUTSINK).
  - Sender με high-resolution waitable timer.
  - Toggle capture και RELEASE στην έξοδο.
- **Exit:** παίζεις στο LAN και τα T1, T4 (LAN) και T9 περνούν.

**Phase 2: Internet και ανθεκτικότητα**
- Status/RTT path και tray (πράσινο/κίτρινο/κόκκινο με ms).
- Auto-release του capture σε link loss.
- Edge redundancy.
- Ctrl+Alt+End για Ctrl+Alt+Del, WTS lock handling.
- Tailscale ACL, bind και allowlist, rate limits.
- Έλεγχος direct path με `tailscale ping`.
- **Exit:** περνούν τα T4 (Internet), T5, T6 και T8.

**Phase 3: Πληρότητα**
- NKRO (ITF2 σε στυλ QMK).
- Horizontal wheel.
- Caps/Num του gaming PC στο tray.
- Soft-detach, διακόπτης ARM.
- WoL από το Pi (client command `wake`).
- `flash_pico.sh` μέσω SWD.
- Fallback για mouse swallow, αν αποτύχει το σχετικό 🧪.
- **Exit:** περνούν τα T2 και T3.

**Phase 4: Stable v1.0**
- Soak 8 ωρών (T7).
- Latency report σε LAN και 4G.
- Config validation, autostart για το client.
- `docs/` πλήρη.
- `/security-review` και `/code-review`.
- Προαιρετικά e2e MAC.

## Verification

- **T0, anti-cheat:** Device Manager δείχνει «HID Keyboard Device» και «HID-compliant mouse» χωρίς εγκατάσταση driver. Notepad: το BOOTSEL γράφει «wwww». Μέσα στο AION 2: ο χαρακτήρας περπατά και η κάμερα γυρίζει. Session 30 λεπτών χωρίς warning ή kick.
- **T1, USB:** Το USBTreeView δείχνει bInterval=1 και 2 interfaces. Το bench του client μετρά inter-arrival των mouse reports περίπου 1 ms.
- **T2, BIOS/UEFI:** Το keyboard δουλεύει στο setup. **T3:** Login screen, UAC και Ctrl+Alt+Del μέσω Ctrl+Alt+End.
- **T4, latency με loopback bench:** Το Pico συνδέεται στο **ίδιο** το remote PC, όχι στο gaming PC. Το `hidway-client --bench` στέλνει F24 και μετρά την άφιξή του μέσω Raw Input, φιλτράροντας το Pico με βάση το hDevice. Έτσι υπάρχει ένα ρολόι και η μέτρηση είναι ακριβής.
  - LAN: στόχος p50 <5 ms, p99 <10 ms.
  - Internet: laptop σε 4G hotspot, με το Pico δίπλα του.
- **T5, impairment:** clumsy στο remote PC (5% loss, 30 ms jitter, reorder). Κανένα stuck key, και η κίνηση μένει σωστή σύμφωνα με το catch-up policy.
- **T6, failure injection, κρατώντας πατημένο το W:**
  - kill του client
  - αποσύνδεση του Ethernet στο remote
  - `systemctl stop hidwayd`
  - αποσύνδεση του probe
  - reboot του Pi
  - reset του Pico
  - `tailscale down`

  Σε κάθε περίπτωση: release ≤250 ms και αυτόματο recovery.
- **T7, soak 8 ωρών:** `fake_client.py` μόνο με ακίνδυνα usages (F13–F24, mouse ±1). Μηδέν watchdog resets, μηδέν stuck states, CRC errors κάτω από το όριο.
- **T8, Steam:** Στο desktop δεν υπάρχει διπλή κίνηση ή διπλό πάτημα. Αν υπάρχει, fallback σε Sunshine + Moonlight με `keyboard = disabled`, `mouse = disabled`.
- **T9, fidelity:** Γνωστά counts στο client δίνουν ακριβώς τα ίδια counts από Raw Input στο bench.
- **Unit tests:** `ctest` για `common/` (COBS round-trip και fuzz, CRC vectors, keymap, int32 wrap στο catch-up).

## Πηγές
- NCSOFT, μέτρα κατά των bots στο AION 2: https://about.ncsoft.com/en/news/article/aion2_update_260128
- Block των G Hub, Synapse και iCUE και το rollback: https://boosting-ground.com/aion-2/news/bot-crisis-gaming-ban-fail
- Virtual input στο AION 2 (Apollo #1258): https://github.com/ClassicOldSong/Apollo/issues/1258
- TinyUSB HID boot keyboard + mouse: https://docs.tinyusb.org/en/latest/examples/device/hid_boot_interface.html
- Sunshine configuration (keyboard/mouse): https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2configuration.html
- Raspberry Pi OTG whitepaper: https://pip-assets.raspberrypi.com/categories/685-app-notes-guides-whitepapers/documents/RP-009276-WP/Using-OTG-mode-on-Raspberry-Pi-SBCs
- cyw43 UDP stall issue: https://github.com/georgerobotics/cyw43-driver/issues/111
- LowLevelKeyboardProc, timeout και αφαίρεση hook: https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelkeyboardproc
- Raw Input: https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input
- WireGuard whitepaper: https://www.wireguard.com/papers/wireguard.pdf
- Tailscale ACLs: https://tailscale.com/kb/1018/acls
- Raspberry Pi Debug Probe: https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html
- Raspberry Pi USB PID allocation: https://github.com/raspberrypi/usb-pid
- HID Usage Tables 1.5: https://usb.org/document-library/hid-usage-tables-15
