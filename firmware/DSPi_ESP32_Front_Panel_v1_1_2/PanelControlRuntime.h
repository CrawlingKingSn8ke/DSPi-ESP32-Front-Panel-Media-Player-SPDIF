#ifndef DSPI_PANEL_CONTROL_RUNTIME_H
#define DSPI_PANEL_CONTROL_RUNTIME_H
#include "PanelControlMenus.h"
#include "PanelNumberEditor.h"
// Sketch integration declarations; all definitions are late in the .inc, after
// the existing transport, list renderer and guarded preset/media machinery.
bool panelOwnsPage(MenuPage page);
String panelPageTitle(MenuPage page);
uint8_t panelMenuCount(MenuPage page);
String panelMenuName(MenuPage page,uint8_t row);
String panelMenuValue(MenuPage page,uint8_t row);
const char *panelMenuUnit(MenuPage page,uint8_t row);
int8_t panelMainIndex(MenuPage page);
void panelEnterPage(MenuPage page);
bool panelSelect();
bool panelWorkPending();
bool panelBack();
void panelCancel();
void panelAdjust(int direction);
void panelApply();
void panelInvalidate();
void panelSourceChanged();
void panelNotification(const uint8_t *payload,uint16_t bytes);
void panelPresetChanged(uint8_t slot);
void panelPresetLoaded(uint8_t slot);
bool panelSaveRoles(uint8_t slot);
void panelLegacySaveCapture(uint8_t slot);
bool panelLegacySaveComplete(uint8_t slot,bool confirmed);
bool panelPersistLocal();
void servicePanelControls();
bool panelNameMove(int direction);
String panelNameWindow();
int16_t panelNameCursorOffset();
int16_t panelNameCursorWidth();
bool panelEditUnderline(MenuPage page,uint8_t row);
int16_t panelNumberCursorOffset();
int16_t panelNumberCursorWidth();
String panelSourceLabel(uint8_t source,const String &fallback,bool shortName);
#endif
