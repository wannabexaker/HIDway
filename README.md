# HIDway

Απομακρυσμένο **φυσικό** keyboard και mouse. Το gaming PC βλέπει μόνο ένα κανονικό USB HID keyboard και mouse, το Raspberry Pi Pico W. Το input έρχεται από το remote PC μέσω Tailscale και Raspberry Pi 4. Δεν τρέχει κανένα software και δεν χρειάζεται κανένας driver στο gaming PC.

```
remote PC (hidway-client) → Tailscale → Pi 4 (hidwayd) → UART → Pico W (USB HID) → gaming PC
```

Η αρχιτεκτονική, οι αποφάσεις και τα ρίσκα βρίσκονται στο [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Κατάσταση: Phase 0 (go/no-go)

Πριν χτιστεί οτιδήποτε άλλο, ελέγχουμε το εξής: **δέχεται το AION 2 input από το Pico ως USB HID;**

Το firmware του T0 στέλνει «W» και κίνηση mouse προς τα δεξιά, όσο κρατάς πατημένο το κουμπί **BOOTSEL** του Pico. Δεν χρειάζονται Pi ή δίκτυο.

### 1. Flash

Το flash μπορεί να γίνει σε οποιοδήποτε PC. Μετά μεταφέρεις το Pico στο gaming PC.

1. Κράτα πατημένο το **BOOTSEL** και σύνδεσε το Pico W με USB. Εμφανίζεται ένας δίσκος `RPI-RP2`.
2. Αντέγραψε το `build\firmware\hidway_fw.uf2` στον δίσκο. Το Pico κάνει επανεκκίνηση μόνο του.

Για να ξανακάνεις flash αργότερα, επανέλαβε τα ίδια βήματα. Το BOOTSEL λειτουργεί ως κουμπί bootloader μόνο τη στιγμή που συνδέεις το Pico.

### 2. LED

| LED | Σημασία |
|---|---|
| Αργό αναβόσβημα | περιμένει τον USB host |
| Σταθερό | συνδεδεμένο, έτοιμο |
| Γρήγορο αναβόσβημα | το BOOTSEL είναι πατημένο και στέλνει input |
| Σβηστό | το PC είναι σε sleep |

### 3. Το τεστ (T0)

1. Σύνδεσε το Pico σε **πίσω** θύρα USB του gaming PC.
2. **Device Manager:** Πρέπει να εμφανιστούν τα «HID Keyboard Device» και «HID-compliant mouse» χωρίς εγκατάσταση driver.
3. **Notepad:** Κράτα το BOOTSEL. Πρέπει να γράφει `wwww` και ο κέρσορας να φεύγει γρήγορα προς τα δεξιά.
4. **AION 2:** Μέσα στο game, κράτα το BOOTSEL.
   - Ο χαρακτήρας πρέπει να περπατά μπροστά.
   - Ο κέρσορας ή η κάμερα πρέπει να κινείται. Αν η κάμερα γυρίζει μόνο με πατημένο το δεξί κλικ, κράτα το δεξί κλικ του κανονικού σου mouse ταυτόχρονα.
5. Παίξε περίπου 30 λεπτά με το Pico συνδεδεμένο και πρόσεξε αν εμφανιστεί κάποιο warning ή αποσύνδεση.

**Αποτέλεσμα:**
- Αν το input δουλεύει στο Notepad αλλά **όχι** μέσα στο game, το παιχνίδι φιλτράρει τη συσκευή και σταματάμε εδώ.
- Αν δουλεύει και στα δύο, προχωράμε στην Phase 1, το prototype στο LAN.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

Χρειάζονται τα εξής:
- Visual Studio με το C++ workload (cl, CMake, Ninja).
- Pico SDK 2.3.1 στο `~\.pico-sdk\sdk\2.3.1`.
- Arm GNU toolchain 14.2 στο `~\.pico-sdk\toolchain\14_2_Rel1`.

Είναι η ίδια δομή φακέλων με το VS Code extension «Raspberry Pi Pico».

## Δομή

```
common/    κοινή C λογική (keyboard 6KRO slots, motion accumulator)
firmware/  Pico W firmware (Pico SDK + TinyUSB)
tests/     host unit tests για το common/ (ctest)
tools/     build script
docs/      αρχιτεκτονική
```
