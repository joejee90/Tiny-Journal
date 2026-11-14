#!/usr/bin/env python3
"""Compile the current shutdown function against host ownership shims."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SHIM = '// Exercises the real shutdown function with host-task/client ownership shims.\n#include <cassert>\n#include <array>\n#include <atomic>\n#include <iostream>\n#include <string>\n#include <vector>\n#include "../JournalInput.h"\nusing String = std::string;\nconstexpr int BLE_HS_CONN_HANDLE_NONE=65535;\nstruct Client {int handle=BLE_HS_CONN_HANDLE_NONE;int getConnHandle(){return handle;}};\nstruct NimBLEDevice {static bool result;static int calls;static bool stopped;static bool deinit(bool all){assert(all);++calls;stopped=result;return result;}};\nbool NimBLEDevice::result=true;int NimBLEDevice::calls=0;bool NimBLEDevice::stopped=false;\nbool btShutdownPending=false,btConnectBusy=false,btInitDone=false,btKeyboardEnabled=false,btKeyboardConnected=false;\nClient *btKeyboardClient=nullptr;\nvoid *btKeyboardAdvDevice=nullptr,*btLedChar=nullptr,*btHidReportQueue=nullptr;\nint *btPairInfo=nullptr;\nbool btLedDirty=true,btAnyKeyDown=true,btRepeatEligible=true,btKeyHeld=true,btLinkLost=true,needsRedraw=false;\nunsigned btPairKind=2,btPairStarted=1;int btConnectResult=-1;\nenum {BT_STATE_IDLE,BT_STATE_CONNECTED};int btSetupState=BT_STATE_CONNECTED;\nstd::vector<int>btScanDevices{1},btSources{1};journal::HidMap btReportMap;\nstd::array<bool,256>btHeld;\nString btKeyboardLastError;\nint cleared=0,deleted=0;\nvoid btClearInputEventQueue(){assert(NimBLEDevice::stopped||!btInitDone);++cleared;}\nvoid vQueueDelete(void*){assert(NimBLEDevice::stopped||!btInitDone);++deleted;}\n'
MAIN = '\nint main(){\n Client client;btKeyboardClient=&client;btShutdownPending=true;btConnectBusy=true;btInitDone=btKeyboardEnabled=btKeyboardConnected=true;\n btHidReportQueue=&client;btPairInfo=new int(1);btHeld.fill(true);\n btFinishPowerOff();assert(NimBLEDevice::calls==0&&deleted==0&&btKeyboardEnabled);\n btConnectBusy=false;client.handle=17;btFinishPowerOff();assert(NimBLEDevice::calls==0&&deleted==0);\n client.handle=BLE_HS_CONN_HANDLE_NONE;NimBLEDevice::result=false;btFinishPowerOff();assert(NimBLEDevice::calls==1&&btInitDone&&btShutdownPending&&deleted==0);\n NimBLEDevice::result=true;btFinishPowerOff();assert(NimBLEDevice::calls==2&&!btInitDone&&!btKeyboardEnabled&&!btKeyboardConnected&&!btShutdownPending);\n assert(btKeyboardClient==nullptr&&btHidReportQueue==nullptr&&btPairInfo==nullptr&&btScanDevices.empty()&&btSources.empty()&&deleted==1&&cleared==1);\n for(bool held:btHeld)assert(!held);\n assert(btPairKind==0&&!btLinkLost&&btConnectResult==0&&btSetupState==BT_STATE_IDLE);\n btFinishPowerOff();assert(NimBLEDevice::calls==2);\n std::cout<<"Bluetooth shutdown waits for worker and GAP release before stopping controller and freeing queue\\n";\n}\n'
source = (ROOT / "JournalBluetooth.inl").read_text()
start = source.index("void btFinishPowerOff() {")
end = source.index("\nvoid btKeyboardStartScan()", start)
# The function under test comes from the current implementation, not a copied
# test version. Only the BLE/client/task boundary is replaced with host shims.
code = SHIM.replace('#include "../JournalInput.h"', '#include "' + str(ROOT / "JournalInput.h") + '"') + source[start:end] + MAIN
with tempfile.TemporaryDirectory(prefix="journal-bt-shutdown-") as directory:
    cpp = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    cpp.write_text(code)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-fsanitize=address,undefined", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
