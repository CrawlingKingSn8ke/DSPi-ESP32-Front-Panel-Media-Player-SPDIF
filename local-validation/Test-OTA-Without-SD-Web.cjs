// Execute the firmware's actual control-state function with a minimal DOM stub.
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert/strict');
const header = fs.readFileSync(path.join(__dirname, '../firmware/DSPi_ESP32_Front_Panel_v1_1_2/WifiTransferWeb.h'), 'utf8');
const script = header.split('<script>')[1].split('</script>')[0];
new Function(script); // Parse the complete portal script too.
const controls = script.slice(script.indexOf('function updateControls(){'), script.indexOf('async function refreshStatus('));
let count = 0;
for (const storageAvailable of [null, true, false]) {
  const elements = {};
  const get = id => elements[id] ||= {
    disabled: false, classList: {toggle() {}},
    closest() { return {querySelectorAll: () => [get(id)]}; }
  };
  const context = {
    $: get, document: {querySelectorAll: () => []}, storageAvailable,
    finishingClient: false, firmwareRunning: false, running: false,
    deleting: false, serverAccepting: true, queue: [], serverWriterActive: false,
    finishBusy: false, scanningNetwork: false, scannedNetworks: [], firmwareFile: {}
  };
  vm.runInNewContext(controls + '\nupdateControls();', context);
  assert.equal(get('files').disabled, storageAvailable === false);
  assert.equal(get('mkdirForm').disabled, storageAvailable === false);
  assert.equal(get('firmwareFile').disabled, false);
  assert.equal(get('installFirmware').disabled, false);
  assert.equal(get('finish').disabled, false);
  assert.equal(get('saveNetwork').disabled, false);
  count += 6;
}
console.log(`${count} portal control assertions passed; complete JavaScript parses.`);
