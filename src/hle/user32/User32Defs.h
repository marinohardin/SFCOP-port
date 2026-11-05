// The USER32 constants the module uses, with the values of the Win32 headers. Structures are not
// here: the guest shims read and write them field by field (User32Guest.cpp).
#pragma once

#include <cstdint>

namespace sfcop::hle::user32 {

// ---- messages ----
constexpr std::uint32_t kWmNull = 0x0000;
constexpr std::uint32_t kWmCreate = 0x0001;
constexpr std::uint32_t kWmDestroy = 0x0002;
constexpr std::uint32_t kWmMove = 0x0003;
constexpr std::uint32_t kWmSize = 0x0005;
constexpr std::uint32_t kWmActivate = 0x0006;
constexpr std::uint32_t kWmSetFocus = 0x0007;
constexpr std::uint32_t kWmKillFocus = 0x0008;
constexpr std::uint32_t kWmEnable = 0x000A;
constexpr std::uint32_t kWmSetText = 0x000C;
constexpr std::uint32_t kWmGetText = 0x000D;
constexpr std::uint32_t kWmGetTextLength = 0x000E;
constexpr std::uint32_t kWmPaint = 0x000F;
constexpr std::uint32_t kWmClose = 0x0010;
constexpr std::uint32_t kWmQuit = 0x0012;
constexpr std::uint32_t kWmEraseBkgnd = 0x0014;
constexpr std::uint32_t kWmShowWindow = 0x0018;
constexpr std::uint32_t kWmActivateApp = 0x001C;
constexpr std::uint32_t kWmSetCursor = 0x0020;
constexpr std::uint32_t kWmMouseActivate = 0x0021;
constexpr std::uint32_t kWmGetMinMaxInfo = 0x0024;
constexpr std::uint32_t kWmWindowPosChanged = 0x0047;
constexpr std::uint32_t kWmNcCreate = 0x0081;
constexpr std::uint32_t kWmNcDestroy = 0x0082;
constexpr std::uint32_t kWmNcCalcSize = 0x0083;
constexpr std::uint32_t kWmNcHitTest = 0x0084;
constexpr std::uint32_t kWmNcActivate = 0x0086;
constexpr std::uint32_t kWmGetDlgCode = 0x0087;
constexpr std::uint32_t kWmKeyDown = 0x0100;
constexpr std::uint32_t kWmKeyUp = 0x0101;
constexpr std::uint32_t kWmChar = 0x0102;
constexpr std::uint32_t kWmSysKeyDown = 0x0104;
constexpr std::uint32_t kWmSysKeyUp = 0x0105;
constexpr std::uint32_t kWmSysChar = 0x0106;
constexpr std::uint32_t kWmInitDialog = 0x0110;
constexpr std::uint32_t kWmCommand = 0x0111;
constexpr std::uint32_t kWmSysCommand = 0x0112;
constexpr std::uint32_t kWmMouseMove = 0x0200;
constexpr std::uint32_t kWmLButtonDown = 0x0201;
constexpr std::uint32_t kWmLButtonUp = 0x0202;
constexpr std::uint32_t kWmLButtonDblClk = 0x0203;
constexpr std::uint32_t kWmRButtonDown = 0x0204;
constexpr std::uint32_t kWmRButtonUp = 0x0205;
constexpr std::uint32_t kWmMButtonDown = 0x0207;
constexpr std::uint32_t kWmMButtonUp = 0x0208;
constexpr std::uint32_t kWmMouseWheel = 0x020A;

// MK_* flags in a mouse message's wParam.
constexpr std::uint32_t kMkLButton = 0x0001;
constexpr std::uint32_t kMkRButton = 0x0002;
constexpr std::uint32_t kMkShift = 0x0004;
constexpr std::uint32_t kMkControl = 0x0008;
constexpr std::uint32_t kMkMButton = 0x0010;

// WM_ACTIVATE states.
constexpr std::uint32_t kWaInactive = 0;
constexpr std::uint32_t kWaActive = 1;
constexpr std::uint32_t kWaClickActive = 2;

// WM_SYSCOMMAND.
constexpr std::uint32_t kScClose = 0xF060;

// PeekMessage flags.
constexpr std::uint32_t kPmRemove = 0x0001;

// Hit tests and mouse activation.
constexpr std::uint32_t kHtClient = 1;
constexpr std::uint32_t kMaActivate = 1;

// ---- window styles ----
constexpr std::uint32_t kWsPopup = 0x80000000u;
constexpr std::uint32_t kWsChild = 0x40000000u;
constexpr std::uint32_t kWsMinimize = 0x20000000u;
constexpr std::uint32_t kWsVisible = 0x10000000u;
constexpr std::uint32_t kWsDisabled = 0x08000000u;
constexpr std::uint32_t kWsCaption = 0x00C00000u; // WS_BORDER | WS_DLGFRAME
constexpr std::uint32_t kWsBorder = 0x00800000u;
constexpr std::uint32_t kWsDlgFrame = 0x00400000u;
constexpr std::uint32_t kWsThickFrame = 0x00040000u;
constexpr std::uint32_t kWsTabStop = 0x00010000u;
constexpr std::uint32_t kWsExDlgModalFrame = 0x00000001u;

// CreateWindowEx's CW_USEDEFAULT.
constexpr std::int32_t kCwUseDefault = static_cast<std::int32_t>(0x80000000u);

// GetWindowLong indices.
constexpr std::int32_t kGwlWndProc = -4;
constexpr std::int32_t kGwlHInstance = -6;
constexpr std::int32_t kGwlHwndParent = -8;
constexpr std::int32_t kGwlId = -12;
constexpr std::int32_t kGwlStyle = -16;
constexpr std::int32_t kGwlExStyle = -20;
constexpr std::int32_t kGwlUserData = -21;
constexpr std::int32_t kDwlMsgResult = 0;
constexpr std::int32_t kDwlDlgProc = 4;
constexpr std::int32_t kDwlUser = 8;

// ShowWindow commands.
constexpr std::uint32_t kSwHide = 0;
constexpr std::uint32_t kSwShowNormal = 1;
constexpr std::uint32_t kSwShowMinimized = 2;
constexpr std::uint32_t kSwShowMaximized = 3;
constexpr std::uint32_t kSwShowNoActivate = 4;
constexpr std::uint32_t kSwShow = 5;
constexpr std::uint32_t kSwMinimize = 6;
constexpr std::uint32_t kSwShowMinNoActive = 7;
constexpr std::uint32_t kSwShowNa = 8;
constexpr std::uint32_t kSwRestore = 9;

// SetWindowPos flags.
constexpr std::uint32_t kSwpNoSize = 0x0001;
constexpr std::uint32_t kSwpNoMove = 0x0002;
constexpr std::uint32_t kSwpNoActivate = 0x0010;
constexpr std::uint32_t kSwpShowWindow = 0x0040;
constexpr std::uint32_t kSwpHideWindow = 0x0080;

// ---- dialogs and controls ----
constexpr std::uint32_t kIdOk = 1;
constexpr std::uint32_t kIdCancel = 2;
constexpr std::uint32_t kIdAbort = 3;
constexpr std::uint32_t kIdRetry = 4;
constexpr std::uint32_t kIdYes = 6;
constexpr std::uint32_t kIdNo = 7;
constexpr std::uint32_t kDsSetFont = 0x40;
constexpr std::uint32_t kBsDefPushButton = 0x01;
constexpr std::uint32_t kBsTypeMask = 0x0F;
constexpr std::uint32_t kBsCheckBox = 0x02;
constexpr std::uint32_t kBsAutoCheckBox = 0x03;
constexpr std::uint32_t kBsGroupBox = 0x07;
constexpr std::uint32_t kEsPassword = 0x0020;
constexpr std::uint32_t kEsNumber = 0x2000;
constexpr std::uint32_t kSsCenter = 0x01;
constexpr std::uint32_t kSsRight = 0x02;
constexpr std::uint32_t kBnClicked = 0;
constexpr std::uint32_t kEnChange = 0x0300;
constexpr std::uint32_t kEmLimitText = 0x00C5;
constexpr std::uint32_t kBmGetCheck = 0x00F0;
constexpr std::uint32_t kBmSetCheck = 0x00F1;

// MessageBox types and their buttons.
constexpr std::uint32_t kMbTypeMask = 0x0F;
constexpr std::uint32_t kMbOk = 0;
constexpr std::uint32_t kMbOkCancel = 1;
constexpr std::uint32_t kMbYesNo = 4;
constexpr std::uint32_t kMbYesNoCancel = 3;
constexpr std::uint32_t kMbRetryCancel = 5;

// ---- virtual keys ----
constexpr std::uint32_t kVkLButton = 0x01;
constexpr std::uint32_t kVkRButton = 0x02;
constexpr std::uint32_t kVkMButton = 0x04;
constexpr std::uint32_t kVkBack = 0x08;
constexpr std::uint32_t kVkTab = 0x09;
constexpr std::uint32_t kVkReturn = 0x0D;
constexpr std::uint32_t kVkShift = 0x10;
constexpr std::uint32_t kVkControl = 0x11;
constexpr std::uint32_t kVkMenu = 0x12;
constexpr std::uint32_t kVkCapital = 0x14;
constexpr std::uint32_t kVkEscape = 0x1B;
constexpr std::uint32_t kVkSpace = 0x20;
constexpr std::uint32_t kVkLeft = 0x25;
constexpr std::uint32_t kVkRight = 0x27;
constexpr std::uint32_t kVkDelete = 0x2E;
constexpr std::uint32_t kVkHome = 0x24;
constexpr std::uint32_t kVkEnd = 0x23;
constexpr std::uint32_t kVkF4 = 0x73;
constexpr std::uint32_t kVkLShift = 0xA0;
constexpr std::uint32_t kVkRShift = 0xA1;
constexpr std::uint32_t kVkLControl = 0xA2;
constexpr std::uint32_t kVkRControl = 0xA3;
constexpr std::uint32_t kVkLMenu = 0xA4;
constexpr std::uint32_t kVkRMenu = 0xA5;

// ---- system metrics ----
constexpr std::uint32_t kSmCxScreen = 0;
constexpr std::uint32_t kSmCyScreen = 1;
constexpr std::uint32_t kSmCxVScroll = 2;
constexpr std::uint32_t kSmCyHScroll = 3;
constexpr std::uint32_t kSmCyCaption = 4;
constexpr std::uint32_t kSmCxBorder = 5;
constexpr std::uint32_t kSmCyBorder = 6;
constexpr std::uint32_t kSmCxDlgFrame = 7;
constexpr std::uint32_t kSmCyDlgFrame = 8;
constexpr std::uint32_t kSmCxIcon = 11;
constexpr std::uint32_t kSmCyIcon = 12;
constexpr std::uint32_t kSmCxCursor = 13;
constexpr std::uint32_t kSmCyCursor = 14;
constexpr std::uint32_t kSmCyMenu = 15;
constexpr std::uint32_t kSmCxFullScreen = 16;
constexpr std::uint32_t kSmCyFullScreen = 17;
constexpr std::uint32_t kSmMousePresent = 19;
constexpr std::uint32_t kSmSwapButton = 23;
constexpr std::uint32_t kSmCxFrame = 32;
constexpr std::uint32_t kSmCyFrame = 33;
constexpr std::uint32_t kSmCxDoubleClk = 36;
constexpr std::uint32_t kSmCyDoubleClk = 37;
constexpr std::uint32_t kSmCMouseButtons = 43;
constexpr std::uint32_t kSmCxVirtualScreen = 78;
constexpr std::uint32_t kSmCyVirtualScreen = 79;
constexpr std::uint32_t kSmCMonitors = 80;

// The frame sizes this module gives windows (and reports as metrics).
constexpr std::int32_t kCaptionHeight = 18;
constexpr std::int32_t kBorderSize = 1;
constexpr std::int32_t kDlgFrameSize = 3;
constexpr std::int32_t kFrameSize = 4;
constexpr std::int32_t kMenuBarHeight = 19;

// ---- display settings ----
constexpr std::uint32_t kEnumCurrentSettings = 0xFFFFFFFFu;
constexpr std::uint32_t kEnumRegistrySettings = 0xFFFFFFFEu;
constexpr std::uint32_t kDispChangeSuccessful = 0;
constexpr std::uint32_t kDispChangeBadMode = 0xFFFFFFFEu; // -2

// ---- SystemParametersInfo ----
constexpr std::uint32_t kSpiGetMouse = 0x0003; // three DWORDs: two thresholds and the speed
constexpr std::uint32_t kSpiGetScreenSaveActive = 0x0010;
constexpr std::uint32_t kSpiSetScreenSaveActive = 0x0011;
constexpr std::uint32_t kSpiGetWorkArea = 0x0030;
constexpr std::uint32_t kSpiSetScreenSaverRunning = 0x0061;
constexpr std::uint32_t kSpiGetMouseSpeed = 0x0070; // one UINT, 1 to 20
constexpr std::uint32_t kErrorInvalidParameter = 87;

// ---- hooks ----
constexpr std::int32_t kWhKeyboardLl = 13;

// The window GetDesktopWindow returns: a constant, never one of this module's windows.
constexpr std::uint32_t kDesktopWindow = 0x00010010u;

// LoadCursorA's system cursors (IDC_ARROW and the rest) and the handles this module gives them.
constexpr std::uint32_t kIdcArrow = 32512;
constexpr std::uint32_t kCursorBase = 0x00020000u;

} // namespace sfcop::hle::user32
