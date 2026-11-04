/* ---------------------------------------------------------------
 *  OLED 表情挂件  —  ESP32-C3 SuperMini + 0.96" SSD1306 + TTP223
 *
 *  表情引擎移植自 TechTalkies 的《Tiny OLED Keychain》
 *    https://github.com/TechTalkies/Youtube-2/tree/main/123_Keychain
 *  原版硬件：XIAO ESP32 + 1.3" 128x64 OLED(SH1106)，纯自动情绪轮播
 *
 *  移植改动：
 *    1. 驱动 SH1106 → SSD1306（0.96" 模块，地址 0x3C）
 *    2. I2C 引脚 4=SCL / 3=SDA（C3 SuperMini 丝印 4/3）
 *    3. TTP223 触摸：7→VCC(IO 供电)、6→OUT、GND→G，极性上电自适应
 *    4. 待机：25s 犯困、30s 睡着
 *    5. 犯困时右上角冒 z，睡着后 z 越来越大、冒得越来越快（打呼噜）
 *    6. 清醒时触摸 → 左右两侧冒小爱心
 *    7. 震动传感器（高有效）：清醒时晃它 → DIZZY 晕；
 *       犯困/睡着时晃它 → INVESTIGATE 打量一会儿再转 ANGRY 生气。
 *       一次晃动会吐出一串脉冲，这里把它们合成同一个事件，不重复触发。
 *
 *  原版核心（算法未改）：
 *    · 眼睛/嘴都是同一种圆角矩形图元，靠缓动逼近目标值 → 丝滑
 *    · EASING=0.25、ROUNDNESS=0.45、30fps 上限
 *    · 6 种情绪 + 随机眨眼(2~6s) + 随机扫视(saccade)
 * ------------------------------------------------------------- */

#include <U8g2lib.h>
#include <Wire.h>
#include <math.h>
#include <WiFi.h>

// 串口命令缓冲（一行一条，和网页共用 execCmd）
char    gSerialCmd[16];
uint8_t gSerialCmdLen = 0;

// 调试：收到 P 命令就把当前帧缓冲 dump 成 hex（用于逐表情排查绘制 bug）
bool gDumpReq = false;

// 表情枚举必须放最前面：后面的 Wi-Fi / 网络段都会引用
enum Emotion {
  NEUTRAL, HAPPY, SAD, SURPRISED, SLEEPY, ANGRY, SLEEPING, YAWN,
  // 由 emote-assets 的 20 个资源名归纳而来（128x64 单色重绘）
  LAUGH, CRY, SHY, THINK, YUMMY, MOCK, MUSIC, CONFIDENT,
  // 第二批：emote-assets 里还没实现的语义
  PANIC, DIZZY, LOVE, SNEEZE, HELLO, INVESTIGATE,
  EMO_COUNT
};

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64

// ---------------- 引脚 ----------------
#define PIN_SCL   4
#define PIN_SDA   3
#define PIN_TVCC  7      // 给 TTP223 供电（IO 输出高，2mA 级）
#define PIN_TOUT  6      // 读 TTP223 输出（A 键）
#define PIN_TINB  10     // B 键：第二路 TTP223 输出（高电平有效）
#define PIN_VIB   0      // 震动传感器输出（高电平有效）—— 换脚只改这里

// ---------------- 双键触摸状态 ----------------
// 一颗按键的完整状态：去抖 + 长按 + 双击 + 按下瞬间的睡眠快照
struct TouchKey {
  uint8_t  pin;
  bool     activeHigh;       // 本路极性：true = 高电平算按下
  bool     cur;              // 去抖后的电平
  bool     prev;             // 上一帧
  bool     raw;              // 原始电平
  unsigned long changeAt;    // 上次电平变化时刻（去抖用）
  unsigned long downAt;      // 本次按下时刻
  bool     longFired;        // 本次已触发长按
  unsigned long releaseAt;   // 松手时刻（0 = 没有待定单击）
  bool     downSleepy;       // 按下那一刻是否在犯困/睡觉（快照）
  uint16_t noiseCount;       // 2 秒内的电平跳变次数（硬件抖动检测）
  unsigned long noiseAt;     // 上次跳变时刻
  bool     noiseWarned;      // 已经就抖动告警过
};

// 两路极性分开存：A 靠开机自动探测，B 因为有内部下拉，物理上只可能是高有效
TouchKey keys[2] = {
  { PIN_TOUT, true, LOW, LOW, LOW, 0, 0, false, 0, false, 0, 0, false },   // 0 = A
  { PIN_TINB, true, LOW, LOW, LOW, 0, 0, false, 0, false, 0, 0, false }    // 1 = B
};

bool          gLocked      = false;  // A+B 长按 2s 锁定
bool          gLockFired   = false;  // 本次组合已触发
unsigned long gLockBadgeUntil = 0;   // 锁标显示到什么时候（切换后 1.5 秒）
unsigned long gClockBadgeUntil = 0;  // 闹钟标显示到什么时候（双击 B 后 1.5 秒）
bool          gClockBadgeOn   = true; // 这次提示是「开」还是「关」
bool          gComboActive = false;
unsigned long gComboAt     = 0;
bool          gLoopSleep   = false;  // B 双击：犯困/睡着后 1 分钟自动醒来重来
unsigned long gLoopStart   = 0;      // 本轮 60 秒倒计时的起点（0 = 没在计时）
const unsigned long LOOP_SLEEP_MS = 60000;   // 圈走完 → 醒来
unsigned long gWakeHappyAt = 0;      // 醒来后 SURPRISED -> HAPPY 的时刻
unsigned long gSleepAt     = 0;      // 打哈欠结束后再过 5 秒才真正睡着
uint8_t       gBright      = 200;    // 当前对比度（跟 setContrast 同步，网页要读）

// ---------------- 空闲随机表演 ----------------
// 最后一次触摸后，先从 18 个轮播表情里随机抽 3 个依次播放；
// 播完再加 50% 概率抽 3 个（与前一组不重复），若触发则再加 50% 概率抽第三组，
// 所以一轮可能是 3 / 6 / 9 个。全部播完且期间没人碰，才走「打哈欠 → 犯困 → 睡着」。
const unsigned long IDLE_FIRST_MS = 3000;   // 触摸后多久开始播第一个
const unsigned long IDLE_FACE_MS  = 7333;   // 每个表情停留多久（3 + 7.33×3 ≈ 25s）
const uint8_t     IDLE_MAX       = 9;       // 一轮最多 3 组
uint8_t       gIdlePick[IDLE_MAX] = {0};    // 本轮抽中的轮播下标
uint8_t       gIdleTotal   = 3;             // 本轮实际要播几个（3 / 6 / 9）
uint8_t       gIdleShowIdx = 255;           // 已播到第几个（255 = 尚未开演）
unsigned long gIdleShowAt   = 0;            // 下一个切换时刻
unsigned long gIdleSeenTouch = 1;           // 上次看到的 gLastTouch（用于检测"有人碰过"）
bool          gIdleInit     = false;

// A 长按是两段式：先 NEUTRAL + 3 心，700ms 后转 LOVE + 再 3 心
unsigned long gLoveAt       = 0;

// ---------------- 屏 ----------------
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset= */ U8X8_PIN_NONE,
                                        /* clock= */ PIN_SCL, /* data= */ PIN_SDA);

// ---------------- 动画参数（原版值） ----------------
const float EASING    = 0.25;
const float ROUNDNESS = 0.45;
const unsigned long FRAME_MS = 33;          // ~30fps
const float EYE_CY   = 26;
const float MOUTH_CY = 55;

// ---------------- 交互节奏 ----------------
const unsigned long DEBOUNCE_MS   = 70;      // 触摸去抖
const unsigned long LONG_PRESS_MS = 1200;    // 按住多久算长按
const unsigned long DOUBLE_TAP_MS = 300;     // 松手后多久内再按算双击
const unsigned long YAWN_MS       = 900;     // 打哈欠过渡时长
const unsigned long HIC_MS        = 50;      // 打嗝（约 3 帧）
const unsigned long DROWSY_MS   = 25000;    // 25s 无人理 → 犯困
// 睡着时刻不在这里定：由 gSleepAt = 打哈欠时刻 + YAWN_MS + 5000 决定
// （抽到 6/9 个随机表演时整段会拉长到 47/69 秒，但「哈欠 → 眯 5 秒 → 睡着」的节奏不变）

bool isBlinking = false;

// TTP223 输出极性：市面模块正逻辑(摸=高)和反相(摸=低)都有，
// 上电时自动探测"未触摸电平"，避免把"松手"当成触摸
bool gTouchActiveHigh = true;
#define TOUCH_TRACE 1      // 1=串口打印每次电平翻转，用来量真实延迟

// 缓动系数：平时 0.25（原版手感），触摸瞬间拉到 0.55，
// 让新表情 2~3 帧（约 70~100ms）就落位
float gEase = EASING;
unsigned long gBoostUntil = 0;

// 手势状态机（单键时代的 gTouchDown/gPressStart/gLongFired/gTapReleaseAt
// 已由文件顶部的 keys[] 取代）
unsigned long gYawnUntil    = 0;       // 打哈欠结束时刻
unsigned long gHicUntil     = 0;       // 打嗝结束时刻

// ---------------- 震动传感器 ----------------
// 一次晃动会吐出一串高电平脉冲。这里的规则是：
//   只要相邻脉冲的间隔小于 VIB_HOLD_MS，就都算同一次晃动；
//   最后一个脉冲之后再过 VIB_HOLD_MS 没有新脉冲，才把脸还回去。
// 震动全程不碰 gLastTouch / lastActionTime —— 晃它不等于「有人理它」，
// 所以不会推迟犯困、也不会打乱随机表演的轮次。
const unsigned long VIB_DEBOUNCE_MS = 40;     // 单个脉冲去抖（接触抖动比这快得多）
const unsigned long VIB_HOLD_MS     = 2000;   // 最后一跳之后表情再留多久（DIZZY 要长一点）
const unsigned long VIB_TOUCH_GUARD = 1200;   // 触摸后这段时间内震动不抢画面（要盖住 A 长按的 LOVE 段）
const unsigned long VIB_INVEST_MS   = 1900;   // 困/睡时：打量多久后转生气

bool          gVibRaw      = false;   // 原始电平
bool          gVibLevel    = false;   // 去抖后的电平
unsigned long gVibChangeAt = 0;       // 电平变化时刻（去抖用）
unsigned long gVibPulseAt  = 0;       // 最近一次有效脉冲的时刻
unsigned long gVibStartAt  = 0;       // 本次震动的起点（打量计时用）
uint8_t       gVibStage    = 0;       // 0=没在震 1=打量 2=生气 3=DIZZY

// =================================================================
//  Wi-Fi 热点 + 网页控制台（不需要装 App、不需要路由器）


// ---------------- 心形位图（5x5 / 7x6，PROGMEM） ----------------
// 每行 1 字节，高位在左：5 列用 bit7..bit3，7 列用 bit7..bit1
static const unsigned char HEART5[5] PROGMEM = { 0x50, 0xF8, 0xF8, 0x70, 0x20 };
static const unsigned char HEART7[6] PROGMEM = { 0x6C, 0xFE, 0xFE, 0x7C, 0x38, 0x10 };

// ---------------- 打呼噜的 z（三档位图，PROGMEM） ----------------
// 用位图而不是 drawLine：手绘 Z 在小尺寸下三条线会互相重叠、糊成一根竖杠
static const unsigned char ZEE5[5]  PROGMEM = { 0xF8, 0x10, 0x20, 0x40, 0xF8 };
static const unsigned char ZEE7[7]  PROGMEM = { 0xFE, 0x02, 0x04, 0x08, 0x10, 0x20, 0xFE };
static const unsigned char ZEE9[18] PROGMEM = {
  0xFF, 0x80,  0x01, 0x00,  0x02, 0x00,  0x04, 0x00,  0x08, 0x00,
  0x10, 0x00,  0x20, 0x00,  0x40, 0x00,  0xFF, 0x80
};

// ---------------- 图元（原版结构） ----------------
struct Eye {
  float x, y, w, h;      // 当前渲染值
  float tx, ty, tw, th;  // 情绪目标值
  float ox, oy;          // 扫视偏移（单独存，不累积）
  bool blinks = true;    // 嘴设为 false

  void snap() {
    x = tx + ox;
    y = ty + oy;
    w = tw;
    h = th;
  }

  void update() {
    float eh = (blinks && isBlinking) ? 2 : th;
    x += (tx + ox - x) * gEase;
    y += (ty + oy - y) * gEase;
    w += (tw - w) * gEase;
    h += (eh - h) * gEase;
  }

  void draw() {
    int ww = max(2, (int)roundf(w));
    int hh = max(2, (int)roundf(h));
    int left = (int)roundf(x - w / 2.0f);
    int top  = (int)roundf(y - h / 2.0f);

    left = constrain(left, 0, SCREEN_WIDTH - ww);
    top  = constrain(top, 0, SCREEN_HEIGHT - hh);

    // u8g2 要求 w,h >= 2*(r+1)，否则 drawRBox 画不出来
    int minSide = min(ww, hh);
    int r = (int)(minSide * ROUNDNESS);
    r = min(r, minSide / 2 - 1);
    if (r < 0) r = 0;

    u8g2.drawRBox(left, top, ww, hh, r);
  }
};

Eye leftEye, rightEye, mouth;

// ---------------- 眉毛（新增：靠它拉开表情辨识度） ----------------
// drop = 内侧相对外侧的下沉量：正值=内侧压低(凶)，负值=内侧抬高(委屈)
struct Brow {
  float x, y, half, drop;
  float tx, ty, thalf, tdrop;
  bool  show  = true;
  bool  isLeft = true;

  void snap() {
    x = tx; y = ty; half = thalf; drop = tdrop;
  }

  void update() {
    x    += (tx    - x)    * gEase;
    y    += (ty    - y)    * gEase;
    half += (thalf - half) * gEase;
    drop += (tdrop - drop) * gEase;
  }

  void draw() {
    if (!show) return;
    int x1 = (int)roundf(x - half), x2 = (int)roundf(x + half);
    int y1, y2;
    if (isLeft) { y1 = (int)roundf(y);            y2 = (int)roundf(y + drop); }
    else        { y1 = (int)roundf(y + drop);     y2 = (int)roundf(y); }
    // 必须夹到屏幕内：u8g2 的 drawLine 参数是无符号数，
    // 传负坐标会被当成 0xFFFFFFxx，裁剪失控，画出一条贯穿全屏的竖线。
    if (x1 < 0) x1 = 0; if (x1 > 127) x1 = 127;
    if (x2 < 0) x2 = 0; if (x2 > 127) x2 = 127;
    if (y1 < 0) y1 = 0; if (y1 > 63) y1 = 63;
    if (y2 < 0) y2 = 0; if (y2 > 63) y2 = 63;
    // 画 3 条平行线凑出 3px 粗（上下各留 1px 也要夹住）
    int a1 = y1 > 0  ? y1 - 1 : 0, b1 = y2 > 0  ? y2 - 1 : 0;
    int a3 = y1 < 63 ? y1 + 1 : 63, b3 = y2 < 63 ? y2 + 1 : 63;
    u8g2.drawLine(x1, a1, x2, b1);
    u8g2.drawLine(x1, y1, x2, y2);
    u8g2.drawLine(x1, a3, x2, b3);
  }
};

Brow leftBrow, rightBrow;

void setBrows(float lx, float ly, float lh, float ld,
              float rx, float ry, float rh, float rd, bool show) {
  leftBrow.isLeft = true;  rightBrow.isLeft = false;
  leftBrow.tx = lx;  leftBrow.ty = ly;  leftBrow.thalf = lh;  leftBrow.tdrop = ld;
  rightBrow.tx = rx; rightBrow.ty = ry; rightBrow.thalf = rh; rightBrow.tdrop = rd;
  leftBrow.show = show; rightBrow.show = show;
}

// 弯月眼（LAUGH / YUMMY 用，替代胶囊）
bool gArcEyes = false;

void drawArcEye(int cx, int cy, int half, bool up) {
  for (int dx = -half; dx <= half; dx++) {
    float t = (float)dx / (float)half;
    int y = cy - (int)roundf((up ? 3.5f : -3.5f) * (1.0f - t * t));
    u8g2.drawLine(cx + dx, y, cx + dx, y + 1);
  }
}

// 非胶囊眼型：0=胶囊 1=月牙(gArcEyes) 2=螺旋 3=叉 4=心
uint8_t gEyeStyle = 0;

void drawSpiral(int cx, int cy, int r0, int r1) {
  const int n = 56;
  int px = 0, py = 0;
  for (int i = 0; i < n; i++) {
    float t   = (float)i / (n - 1);
    float ang = t * 12.6f;                // 整 2 圈，螺旋更明显
    float r   = r0 + (r1 - r0) * t;
    int x = cx + (int)roundf(r * cosf(ang));
    int y = cy + (int)roundf(r * sinf(ang));
    if (i > 0) u8g2.drawLine(px, py, x, y);
    px = x; py = y;
  }
}

void drawCross(int cx, int cy, int half) {
  u8g2.drawLine(cx - half, cy - half, cx + half, cy + half);
  u8g2.drawLine(cx - half, cy + half, cx + half, cy - half);
  u8g2.drawLine(cx - half + 1, cy - half, cx + half + 1, cy + half);
  u8g2.drawLine(cx - half + 1, cy + half, cx + half + 1, cy - half);
}

// ---------------- 情绪 ----------------
Emotion currentEmotion = NEUTRAL;

// 触摸轮播顺序：第一下落开心（惊讶 → 开心），之后每摸一下换一个
// 触摸轮播：18 个参数化表情，摸一下换一个
const Emotion CYCLE[] = {
  HAPPY, LAUGH, YUMMY, LOVE, SHY, HELLO, THINK, INVESTIGATE,
  CRY, MOCK, CONFIDENT, MUSIC, ANGRY, PANIC, DIZZY, SNEEZE, SAD, NEUTRAL
};
const int CYCLE_N = sizeof(CYCLE) / sizeof(CYCLE[0]);
int gCycleIdx = 0;

// ---------------- 运行时状态 ----------------
bool gDrowsy = false;     // 25s 后犯困
bool gAsleep = false;     // 30s 后睡着
unsigned long gLastTouch  = 0;

//  连上热点 FacePendant → 浏览器开 http://192.168.4.1
// =================================================================
#define WIFI_SSID   "FacePendant"
#define WIFI_PASS   "face1234"      // 至少 8 位
#define WIFI_CHANNEL 6              // ch1 这边挤到 84% 利用率，手机上根本搜不出来
#define WIFI_ALWAYS_ON 1            // 改成 0：60 秒没人访问就关热点省电

WiFiServer    gSrv(80);            // 手写 HTTP，比 WebServer 库听话
bool          gWifiOn = false;
char          gLastEvent[48] = "BOOT";
unsigned long gWifiIdle  = 0;

// 所有"推送"统一走这里；网页端自己轮询 /st，所以这里只需记录最近一条
void netNotify(const char* s) {
  if (s) {
    strncpy(gLastEvent, s, sizeof(gLastEvent) - 1);
    gLastEvent[sizeof(gLastEvent) - 1] = 0;
  }
}

static const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>FacePendant</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{background:#070a0e}body{margin:0;min-height:100vh;background:radial-gradient(circle at 50% 0%,#123040,#070a0e 70%);
 color:#cfe3ff;font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;
 display:flex;flex-direction:column;align-items:center;padding:20px 14px 32px;gap:12px}
#face{font-size:54px;line-height:1;filter:drop-shadow(0 0 18px #37c0ff88)}
h1{margin:0;font-size:13px;letter-spacing:.34em;color:#54c8ff;font-weight:600}
#st{font-size:12px;color:#68849c;text-align:center;font-variant-numeric:tabular-nums;line-height:1.7}
#st b{color:#8fd6ff;font-weight:600}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:7px;width:100%;max-width:380px}
.grid button{min-height:56px;border:1px solid #1d3348;border-radius:13px;background:#0e1a24;
 color:#d6ecff;font-size:11px;font-weight:600;font-family:inherit;padding:4px 2px;
 display:flex;flex-direction:column;align-items:center;justify-content:center;gap:2px;
 transition:background .12s,transform .08s,border-color .12s}
.grid button span.e{font-size:17px;line-height:1}
.grid button:active{background:#1b4159;border-color:#3d8fbd;transform:scale(.94)}
.grid button.on{background:#12405c;border-color:#4fb3e8;color:#bfe9ff}
.row{display:flex;gap:8px;width:100%;max-width:380px}
.row button{flex:1;min-height:46px;border:1px solid #1d3348;border-radius:13px;background:#0e1a24;
 color:#d6ecff;font-size:13px;font-weight:600;font-family:inherit;
 transition:background .12s,transform .08s,border-color .12s}
.row button:active{background:#1b4159;border-color:#3d8fbd;transform:scale(.95)}
.row button.on{background:#12405c;border-color:#4fb3e8}
.ttl{width:100%;max-width:380px;font-size:11px;color:#4e6c84;letter-spacing:.18em;margin:2px 0 -4px}
</style></head><body>
<div id="face">&#128528;</div>
<h1>FACE PENDANT</h1>
<div id="st">连接中…</div>

<div class="ttl">表情</div>
<div class="grid" id="faces"></div>

<div class="ttl">设置</div>
<div class="row">
 <button id="bLoop" onclick="cmd('K')">倒计时</button>
 <button onclick="cmd('B'+Math.max(0,BR-24))">调暗</button>
 <button onclick="cmd('B'+Math.min(255,BR+24))">调亮</button>
</div>
<div class="row">
 <button onclick="cmd('N')">重启屏幕</button>
 <button onclick="cmd('R')">锁定</button>
</div>

<div class="ttl">手势</div>
<div class="row">
 <button onclick="cmd('T')">摸一下</button>
 <button onclick="cmd('L')">长按</button>
 <button onclick="cmd('D')">双击</button>
</div>

<script>
var N=["呆萌","开心","难过","惊讶","犯困","生气","睡着","哈欠","大笑","哭","害羞","思考","好吃","嫌弃","听歌","自信","慌张","晕","爱心","喷嚏","挥手","疑惑"], E=["😐","😄","😢","😮","😪","😠","😴","🥱","😆","😭","😊","🤔","😋","🙄","🎵","😎","😱","😵","🥰","🤧","👋","❓"], BR=200, CUR=-1;
(function(){var g=document.getElementById('faces');
 for(var i=0;i<N.length;i++){(function(i){
   var b=document.createElement('button');
   b.innerHTML='<span class="e">'+E[i]+'</span>'+N[i];
   b.onclick=function(){cmd('F'+i)}; b.id='f'+i; g.appendChild(b);
 })(i)}})();
function cmd(c){fetch('/c?x='+c).then(r=>r.text()).then(poll).catch(function(){})}
function poll(){fetch('/st').then(r=>r.json()).then(function(j){
  document.getElementById('face').textContent=(j.f>=0&&j.f<E.length)?E[j.f]:E[0];
  BR=j.br;
  if(j.f!==CUR){ if(CUR>=0){var o=document.getElementById('f'+CUR); if(o)o.classList.remove('on');}
    var n=document.getElementById('f'+j.f); if(n)n.classList.add('on'); CUR=j.f; }
  var loop=document.getElementById('bLoop');
  loop.textContent=j.lp?('倒计时 '+Math.ceil(j.rm/1000)+'s'):'倒计时';
  loop.className=j.lp?'on':'';
  var st='<b>'+N[j.f]+'</b> #'+j.f+' · 亮度 '+j.br+'<br>';
  st+='上手 '+j.up+'s · 距上次触摸 '+j.t+'s<br>';
  st+='倒计时 '+(j.lp?(j.rm>=0?(Math.ceil(j.rm/1000)+'s'):'待触发'):'关')
     +' · 锁定 '+(j.lk?'是':'否')
     +' · '+(j.as?'睡着':(j.dr?'犯困':'清醒'))+'<br>'+j.e;
  document.getElementById('st').innerHTML=st;
}).catch(function(){document.getElementById('st').textContent='板子已断开'})}
setInterval(poll,700);poll();
</script></body></html>
)HTML";

void wifiInit() {
  WiFi.persistent(false);

  // 关键：先把射频彻底关掉再开 AP。
  // 之前「先 STA 扫描 → 再切 AP」的写法会让 AP 只报告启动成功、却不发 beacon，
  // 手机和电脑都搜不到（实测：ch1 + 扫描后切模式 = 无广播；ch6 + 纯 AP = 正常）
  WiFi.mode(WIFI_OFF);
  delay(300);
  WiFi.mode(WIFI_AP);
  delay(300);

  IPAddress apip(192, 168, 4, 1);
  WiFi.softAPConfig(apip, apip, IPAddress(255, 255, 255, 0));
  bool ok = WiFi.softAP(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL, 0, 4);
  delay(800);
  Serial.printf("[wifi] softAP=%d mode=%d ip=%s ch=%d mac=%s\n",
                (int)ok, (int)WiFi.getMode(),
                WiFi.softAPIP().toString().c_str(), WiFi.channel(),
                WiFi.softAPmacAddress().c_str());
  if (!ok) Serial.println("[wifi] !!! softAP FAILED");
  gWifiOn = ok;
  gWifiIdle = millis();

  gSrv.begin();
  gSrv.setNoDelay(true);

  Serial.printf("[wifi] AP \"%s\"  pass \"%s\"  ->  http://%s\n",
                WIFI_SSID, WIFI_PASS, WiFi.softAPIP().toString().c_str());
  Serial.println("[wifi] open that URL in phone browser");
}


// ---------------- 手写 HTTP（只处理 GET，够用） ----------------
void httpSend(WiFiClient& c, const char* status, const char* ctype,
              const char* body, size_t len) {
  c.printf("HTTP/1.1 %s\r\n", status);
  c.printf("Content-Type: %s\r\n", ctype);
  c.printf("Content-Length: %u\r\n", (unsigned)len);
  c.print("Cache-Control: no-store\r\n");
  c.print("Connection: close\r\n\r\n");

  // 分 512 字节小块推：一次性 write 3KB 会吃满发送缓冲，
  // 没写完就 stop() → 客户端报 "connection aborted"
  size_t sent = 0;
  while (sent < len) {
    size_t chunk = (len - sent > 512) ? 512 : (len - sent);
    size_t n = c.write((const uint8_t*)body + sent, chunk);
    if (n == 0) { delay(5); if (!c.connected()) break; continue; }
    sent += n;
    delay(1);
  }
  c.flush();
  delay(40);                                  // 给 TCP 留出发 FIN 的时间
  c.stop();
  if (sent < len) Serial.printf("[http] short write %u/%u\n", (unsigned)sent, (unsigned)len);
}

void httpPoll(unsigned long now) {
  WiFiClient c = gSrv.available();
  if (!c) return;

  c.setTimeout(2);
  unsigned long t0 = millis();
  while (!c.available() && millis() - t0 < 350) delay(1);
  if (!c.available()) { c.stop(); return; }

  String line = c.readStringUntil('\n');
  line.trim();
  while (c.available()) {                     // 丢掉剩下的 header
    String h = c.readStringUntil('\n');
    if (h.length() <= 1) break;
  }
  Serial.printf("[http] %s  <- %s\n", c.remoteIP().toString().c_str(), line.c_str());

  if (line.startsWith("GET / ") || line == "GET /") {
    httpSend(c, "200 OK", "text/html; charset=utf-8", PAGE_HTML, strlen(PAGE_HTML));
  } else if (line.startsWith("GET /st")) {
    char buf[320];
    unsigned long now = millis();
    long remain = (gLoopSleep && gLoopStart) ? ((long)LOOP_SLEEP_MS - (long)(now - gLoopStart)) : -1;
    if (remain < 0 && gLoopSleep) remain = 0;
    snprintf(buf, sizeof(buf),
             "{\"f\":%d,\"n\":%d,\"up\":%lu,\"t\":%lu,\"e\":\"%s\","
             "\"lp\":%d,\"lk\":%d,\"dr\":%d,\"as\":%d,\"rm\":%ld,\"br\":%d}",
             (int)currentEmotion, (int)EMO_COUNT, now / 1000, gLastTouch / 1000, gLastEvent,
             (int)gLoopSleep, (int)gLocked, (int)gDrowsy, (int)gAsleep, remain, (int)gBright);
    httpSend(c, "200 OK", "application/json; charset=utf-8", buf, strlen(buf));
  } else if (line.startsWith("GET /c?x=")) {
    int p = line.indexOf("?x=") + 3;
    int q = line.indexOf(' ', p);
    String cmd = (q > p) ? line.substring(p, q) : line.substring(p);
    cmd = cmd.substring(0, 15);
    execCmd(cmd.c_str(), now);
    httpSend(c, "200 OK", "text/plain; charset=utf-8", cmd.c_str(), cmd.length());
  } else if (line.startsWith("GET /favicon.ico")) {
    httpSend(c, "404 Not Found", "text/plain", "", 0);
  } else {
    httpSend(c, "404 Not Found", "text/plain", "404", 3);
  }
}

void netPoll(unsigned long now) {
  if (!gWifiOn) return;
  httpPoll(now);

  static unsigned long lastLog = 0;
  static int lastSta = -1;
  int sta = WiFi.softAPgetStationNum();
  if (sta != lastSta) {
    lastSta = sta;
    Serial.printf("[wifi] stations=%d heap=%u\n", sta, (unsigned)ESP.getFreeHeap());
  }
  if (now - lastLog > 15000) {
    lastLog = now;
    Serial.printf("[wifi] alive sta=%d heap=%u\n", sta, (unsigned)ESP.getFreeHeap());
  }

#if !WIFI_ALWAYS_ON
  if (WiFi.softAPgetStationNum() == 0) {
    if (gWifiIdle == 0) gWifiIdle = now;
    else if (now - gWifiIdle > 60000) {       // 60 秒没人连 → 关热点省电
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      gWifiOn = false;
      Serial.println("[wifi] idle 60s, AP off (touch to bring back by power cycle)");
    }
  } else {
    gWifiIdle = 0;
  }
#endif
}
unsigned long gSleepStart = 0;   // 进入睡眠的时刻（用来让呼噜越来越快）

unsigned long lastActionTime = 0;
unsigned long actionInterval = 2000;

unsigned long lastBlinkTime = 0;
unsigned long blinkInterval = 3000;

unsigned long lastFrameTime = 0;

bool gTouchPrev = LOW;
unsigned long gTouchEdge = 0;

// ---------------- 打呼噜的 z ----------------
#define ZEE_MAX 5
struct Zee {
  bool on;
  float x, y;        // 出生位置
  float vx, vy;      // 每帧位移
  float s0;          // 出生尺寸（半宽）
  unsigned long born, life;
};
Zee zees[ZEE_MAX];
unsigned long gNextZeeAt = 0;

// ---------------- 触摸冒的爱心 ----------------
#define HEART_MAX 10
struct Heart {
  bool on;
  float x, y;
  float vx, vy;
  uint8_t big;       // 0=5x5  1=7x6
  unsigned long born, life;
};
Heart hearts[HEART_MAX];

// =================================================================
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println("=== OLED keychain face boot ===");

  // ① 触摸模块上电（TTP223 自校准窗口内别读 OUT）
  pinMode(PIN_TVCC, OUTPUT);
  digitalWrite(PIN_TVCC, HIGH);
  delay(300);

  // ② 触摸输入
  // A 路：TTP223 推挽输出，普通输入；极性靠下面采样自动判
  pinMode(PIN_TOUT, INPUT);
  // B 路：加内部下拉。TTP223 是推挽输出，下拉不影响它拉高；
  // 但万一模块没供电/没接好，输出会浮空，下拉能把它稳定在"未触摸"。
  pinMode(PIN_TINB, INPUT_PULLDOWN);

  // A 路极性：读 12 次，稳定才采用；抖到判不出来就按"高有效"处理（宁可漏触也不误触）
  int hiA = 0;
  for (int i = 0; i < 12; i++) { hiA += digitalRead(PIN_TOUT); delay(4); }
  bool aHigh;
  if (hiA >= 10)      aHigh = false;   // 空闲高 → 摸下去是低
  else if (hiA <= 2)  aHigh = true;    // 空闲低 → 摸是拉高
  else                aHigh = true;    // 不稳：按高有效，绝不把低电平当常按
  keys[0].activeHigh = aHigh;
  // B 路有下拉，物理上只可能是高有效
  keys[1].activeHigh = true;
  Serial.printf("[touch] A idle high=%d/12 -> %s | B 固定 HIGH\n",
                hiA, aHigh ? "HIGH" : "LOW");

  // ③ 上电时把两路的"当前电平"直接当作初始状态，
  //    否则 changeAt 还是 0，(now - 0) > DEBOUNCE_MS 恒成立，
  //    开机那一瞬间的抖动会被当成一次真实的按下 → 启动就蹦出一个单击手势。
  for (int i = 0; i < 2; i++) {
    bool r = digitalRead(keys[i].pin);
    bool c = keys[i].activeHigh ? (r == HIGH) : (r == LOW);
    keys[i].raw = r;
    keys[i].cur = c;
    keys[i].prev = c;
    keys[i].changeAt = millis();
    keys[i].downAt = millis();
    keys[i].releaseAt = 0;
    keys[i].longFired = false;
    keys[i].downSleepy = false;
  }
  Serial.printf("[touch] init A=%d B=%d\n", (int)digitalRead(PIN_TOUT), (int)digitalRead(PIN_TINB));

  // ②.5 震动传感器（高有效）。内部下拉：模块没接好时稳定在"没震"，
  //     不会因为悬空而把整块表情冲到 DIZZY。
  pinMode(PIN_VIB, INPUT_PULLDOWN);
  gVibRaw = gVibLevel = (digitalRead(PIN_VIB) == HIGH);
  gVibChangeAt = millis();
  Serial.printf("[vib] init level=%d\n", (int)gVibLevel);

  // ③ 屏（速率必须在 begin() 之前设：begin 时才把 bus_clock 交给 HAL）
  u8g2.setBusClock(400000);
  Wire.setClock(400000);
  u8g2.begin();
  u8g2.setContrast(200); gBright = 200;

  // ③.5 I2C 体检
  {
    uint8_t found = 0;
    Serial.print("[i2c] devices:");
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        Serial.printf(" 0x%02X", a);
        found++;
      }
    }
    if (!found) Serial.print(" none   <== 屏没接上 / SCL SDA 或 3V3 有问题");
    Serial.println();
  }

  mouth.blinks = false;

  // ④ 开机：闭眼 → 睁眼
  setEmotion(NEUTRAL);
  leftEye.x  = leftEye.tx;
  leftEye.y  = leftEye.ty;
  leftEye.w  = leftEye.tw;
  leftEye.h  = 2.0f;
  rightEye.x = rightEye.tx;
  rightEye.y = rightEye.ty;
  rightEye.w = rightEye.tw;
  rightEye.h = 2.0f;
  mouth.snap();

  unsigned long now = millis();
  gLastTouch  = now;
  lastActionTime = now;
  lastBlinkTime  = now;
  lastFrameTime  = now;

  wifiInit();

  Serial.println("[boot] ready, touch to change face");
}


// =================================================================
//  双键触摸：A(GPIO6) / B(GPIO10)
// =================================================================
// 去抖：电平要稳定 DEBOUNCE_MS 才认
void pollKey(int i, unsigned long now) {
  TouchKey &k = keys[i];
  bool r = digitalRead(k.pin);
  if (r != k.raw) {                       // 原始电平变了 → 计入噪声统计
    k.raw = r;
    k.noiseAt = now;
    k.noiseCount++;
  }
  // 2 秒内没再跳变就清零
  if (k.noiseCount && (now - k.noiseAt) > 2000) k.noiseCount = 0;
  // 抖动保护：2 秒内跳 30 次以上 = 模块没供电/线松了，直接忽略这一路
  if (k.noiseCount > 30) {
    k.cur = false;
    k.prev = false;
    k.releaseAt = 0;
    if (!k.noiseWarned) {
      k.noiseWarned = true;
      Serial.printf("[touch] key %c 抖动异常(%d 次/2s) -> 暂不响应，检查 3V3 与 OUT 接线\n",
                    i ? 'B' : 'A', k.noiseCount);
    }
    return;
  }
  if (k.noiseWarned && k.noiseCount <= 5) {
    k.noiseWarned = false;
    Serial.printf("[touch] key %c 恢复正常\n", i ? 'B' : 'A');
  }
  bool c = k.activeHigh ? (r == HIGH) : (r == LOW);
  if (c != k.cur && (now - k.changeAt) >= DEBOUNCE_MS) {
    k.cur      = c;
    k.changeAt = now;
  }
}

bool isSleepyState() { return gDrowsy || gAsleep; }

// ---------------- 震动：读数 + 表情 ----------------
// 40ms 去抖，只认上升沿。一串脉冲会不断刷新 gVibPulseAt，
// 于是「还在晃」这件事对上层就是一个持续的布尔量。
void pollVib(unsigned long now) {
  bool r = (digitalRead(PIN_VIB) == HIGH);
  if (r != gVibRaw) { gVibRaw = r; gVibChangeAt = now; }
  if (r == gVibLevel) return;
  if ((now - gVibChangeAt) < VIB_DEBOUNCE_MS) return;
  gVibLevel = r;
  if (r) gVibPulseAt = now;
}

// 震动表情：
//   清醒      → DIZZY（螺旋眼），停留 VIB_HOLD_MS
//   犯困/睡着 → INVESTIGATE 打量，VIB_INVEST_MS 之后转 ANGRY
// 触摸永远优先：触摸后 VIB_TOUCH_GUARD 之内完全不出手，
// 等触摸的表情播完，若还在晃再从头开始打量/晕。
void vibLogic(unsigned long now) {
  // 还在晃 = 电平当下就是高，或者最后一跳还在 HOLD 窗口里。
  // 前一条照顾"一直保持高"（倾斜开关/持续摇晃），后一条照顾一串短脉冲。
  bool pulse     = gVibLevel || (gVibPulseAt && (now - gVibPulseAt) < VIB_HOLD_MS);
  bool touchBusy = (now - gLastTouch) < VIB_TOUCH_GUARD;

  if (!pulse || touchBusy) {
    if (!gVibStage) return;
    gVibStage  = 0;
    gVibStartAt = 0;
    if (touchBusy) return;                 // 被触摸接管：表情归触摸那条路管
    // 还脸：别把哈欠、犯困、睡着这三种状态画错
    if (gYawnUntil && now < gYawnUntil) setEmotion(YAWN);
    else if (gAsleep)   setEmotion(SLEEPING);
    else if (gDrowsy)   setEmotion(SLEEPY);
    else              { setEmotion(NEUTRAL); gIdleShowAt = now + 600; }
    Serial.println("[vib] end");
    return;
  }

  bool sleepy = isSleepyState();
  if (gVibStage == 0) {
    gVibStartAt = now;
    gVibStage   = sleepy ? 1 : 3;
    Serial.printf("[vib] start (%s)\n", sleepy ? "sleepy" : "awake");
  } else if (sleepy && gVibStage == 3) {   // 晃着晃着犯困了 → 改走打量
    gVibStage = 1; gVibStartAt = now;
    Serial.println("[vib] became sleepy -> INVESTIGATE");
  } else if (!sleepy && gVibStage != 3) {  // 被叫醒 → 回到晕
    gVibStage = 3;
    Serial.println("[vib] became awake -> DIZZY");
  } else if (gVibStage == 1 && (now - gVibStartAt) >= VIB_INVEST_MS) {
    gVibStage = 2;
    Serial.println("[vib] -> ANGRY");
  }

  // 目标表情（别的地方可能刚覆盖过，比如犯困计时到点切了 YAWN）
  Emotion want = (gVibStage == 1) ? INVESTIGATE : (gVibStage == 2 ? ANGRY : DIZZY);
  if (currentEmotion != want) {
    setEmotion(want);
    gBoostUntil = now + 200;
  }
}

void wakeUpByTouch(unsigned long now) {
  gLastTouch     = now;
  lastActionTime = now;
  gYawnUntil     = 0;
  gSleepAt       = 0;                    // 取消待定的「哈欠后 5 秒睡着」
  gAsleep        = false;
  gDrowsy        = false;
  gSleepStart    = 0;
  gLoopStart     = 0;                    // 手动叫醒 → 这一轮倒计时作废
  for (int i = 0; i < ZEE_MAX; i++) zees[i].on = false;
  isBlinking     = false;
  Serial.println("[touch] wake");
}

// ---------------- A 键 ----------------
void onA_Single(unsigned long now, bool wasSleepy) {
  gLastTouch = now; lastActionTime = now; gYawnUntil = 0;
  if (wasSleepy) {
    // 睡着时摸一下：先醒 → 惊讶 → 300ms 后开心
    wakeUpByTouch(now);
    gLoopStart = 0;                      // 人为叫醒 → 本轮倒计时作废，下次入睡重新起算
    setEmotion(SURPRISED);
    gWakeHappyAt = now + 300;
    gBoostUntil  = now + 300;
    Serial.println("[A] wake -> SURPRISED ... HAPPY");
  } else {
    setEmotion(SHY);
    burstHearts(now);
    gBoostUntil = now + 260;
    Serial.println("[A] single -> SHY + hearts");
  }
}

void onA_Long(unsigned long now, bool wasSleepy) {
  if (wasSleepy) {                       // 睡着时长按：不打扰，继续打呼噜
    Serial.println("[A] long ignored (sleeping)");
    return;
  }
  gLastTouch = now; lastActionTime = now;
  // 两段式：先 NEUTRAL 呆萌 + 3 心，700ms 后转 LOVE 爱心 + 再 3 心
  setEmotion(NEUTRAL);
  burstHearts(now);
  gBoostUntil = now + 260;
  gLoveAt = now + 700;
  Serial.println("[A] long -> NEUTRAL + hearts ... LOVE + hearts");
}

void onA_Double(unsigned long now, bool wasSleepy) {
  if (wasSleepy) {                       // 睡着时双击：同样不打扰
    Serial.println("[A] double ignored (sleeping)");
    return;
  }
  gLastTouch = now; lastActionTime = now;
  setEmotion(HAPPY);                     // 咧大嘴 + 6 颗心从下往上
  burstHeartsUp(now);
  gBoostUntil = now + 240;
  Serial.println("[A] double -> HAPPY + hearts up");
}

// ---------------- B 键 ----------------
void onB_Single(unsigned long now, bool wasSleepy) {
  if (wasSleepy) {                       // B 单击只在清醒时有效
    Serial.println("[B] single ignored (sleeping)");
    return;
  }
  gLastTouch = now; lastActionTime = now;
  setEmotion(CYCLE[gCycleIdx]);
  gCycleIdx = (gCycleIdx + 1) % CYCLE_N;
  gBoostUntil = now + 220;
  Serial.printf("[B] single -> face %d\n", (int)currentEmotion);
}

void onB_Double(unsigned long now, bool wasSleepy) {
  // 犯困 / 睡着 → 一律不打扰：不起表情、不起爱心、不改倒计时（开或关都不动）
  // 要操作倒计时，先按 A 单击把它叫醒
  if (wasSleepy) {
    Serial.println("[B] double ignored (sleeping)");
    return;
  }
  // B 双击 = 只增加/取消一个 60 秒自动重启计时器，不锁任何其它按键
  gLastTouch = now; lastActionTime = now;
  gLoopSleep = !gLoopSleep;
  gClockBadgeOn = gLoopSleep;            // 左上角提示：开=闹钟，关=闹钟带斜杠
  gClockBadgeUntil = now + 1500;
  if (!gLoopSleep) {
    gLoopStart = 0;
    Serial.println("[B] double -> countdown OFF");
  } else if (gDrowsy || gAsleep) {
    gLoopStart = now;                    // 开的时候已经在犯困/睡着 → 从现在起算 60 秒
    Serial.println("[B] double -> countdown ON (already sleepy, start now)");
  } else {
    gLoopStart = 0;                      // 清醒 → 等它这次进入犯困再起算
    Serial.println("[B] double -> countdown ON (will start when drowsy)");
  }
  setEmotion(gLoopSleep ? HAPPY : SHY);
  gBoostUntil = now + 260;
}

// B 长按 = 重启 OLED（重新初始化控制器 + 恢复亮度 + 重画一帧）
void onB_Long(unsigned long now) {
  gLastTouch = now; lastActionTime = now;
  Serial.println("[B] long -> OLED restart");

  u8g2.clearBuffer();
  u8g2.sendBuffer();

  // 重新走一遍控制器初始化；I2C 也会被重新拉起，能救回总线卡死的情况
  u8g2.begin();
  u8g2.setContrast(255);                 // 防止之前被调暗过
  gBright = 255;
  delay(30);

  setEmotion(NEUTRAL);
  gBoostUntil = now + 400;
  Serial.println("[B] long -> OLED restart done");
}

// ---------------- 空闲随机表演 ----------------
// 从 18 个轮播表情里抽互不重复的下标，填到 gIdlePick[idx]
void pickOneIdleFace(int idx) {
  bool dup;
  uint8_t pick;
  do {
    pick = (uint8_t)(esp_random() % (uint32_t)CYCLE_N);
    dup = false;
    for (int j = 0; j < idx; j++) {
      if (gIdlePick[j] == pick) { dup = true; break; }
    }
  } while (dup);
  gIdlePick[idx] = pick;
}

// 抽本轮表演：3 / 6 / 9 三个档位等概率（各约 33.3%）
void pickIdleFaces() {
  uint32_t dice = esp_random() % 3;         // 0=3 个，1=6 个，2=9 个
  gIdleTotal = (uint8_t)(3 * (dice + 1));

  for (int i = 0; i < gIdleTotal; i++) pickOneIdleFace(i);

  Serial.printf("[idle] shuffle -> %d 个:", (int)gIdleTotal);
  for (int i = 0; i < gIdleTotal; i++) {
    Serial.printf(" pos%d=face%d", (int)gIdlePick[i], (int)CYCLE[gIdlePick[i]]);
  }
  Serial.println();
}

// ---------------- 主状态机 ----------------
void touchLogic(unsigned long now) {
  // 上电 1.5 秒内不判手势：TTP223 自校准期间输出会跳，容易误判成单击
  if (now < 1500) {
    for (int i = 0; i < 2; i++) { keys[i].prev = keys[i].cur; keys[i].releaseAt = 0; }
    return;
  }

  // ---- 组合键 A+B 按住 2 秒 = 锁定 / 解锁 ----
  bool both = keys[0].cur && keys[1].cur;
  if (both && !gComboActive) {
    gComboActive = true; gComboAt = now; gLockFired = false;
  }
  if (!both && gComboActive) { gComboActive = false; gComboAt = 0; }
  if (gComboActive && !gLockFired && (now - gComboAt) >= 2000) {
    gLockFired = true;
    gLocked = !gLocked;
    gLockBadgeUntil = now + 1500;      // 左上角闪个小锁，1.5 秒后消失
    // 只锁 A/B 两键的误触，不碰任何计时器和表情状态
    Serial.printf("[lock] %s\n", gLocked ? "ON" : "OFF");
  }
  // 组合按住期间：屏蔽单键，且松手也不能算单击
  if (gComboActive) {
    for (int i = 0; i < 2; i++) {
      keys[i].prev = keys[i].cur;
      keys[i].longFired = true;
      keys[i].releaseAt = 0;
    }
    return;
  }
  // 已锁定：什么都不响应
  if (gLocked) {
    for (int i = 0; i < 2; i++) { keys[i].prev = keys[i].cur; keys[i].releaseAt = 0; }
    return;
  }

  // ---- 单键手势 ----
  for (int i = 0; i < 2; i++) {
    TouchKey &k = keys[i];
    if (k.cur && !k.prev) {              // 按下沿
      k.downAt     = now;
      k.longFired  = false;
      k.downSleepy = isSleepyState();    // 快照：这一刻在不在犯困/睡觉
      gLoveAt      = 0;                  // 任何新按键都取消还没播的 LOVE 段
    }
    if (k.cur && !k.longFired && (now - k.downAt) >= LONG_PRESS_MS) {
      k.longFired = true;                // 长按就地生效（不松手也算）
      if (i == 0) onA_Long(now, k.downSleepy);
      else        onB_Long(now);         // B 长按 = 重启 OLED
    }
    if (!k.cur && k.prev) {              // 松手沿
      if (!k.longFired) {
        bool dbl = (k.releaseAt && (now - k.releaseAt) <= DOUBLE_TAP_MS);
        k.releaseAt = dbl ? 0 : now;
        if (dbl) {
          if (i == 0) onA_Double(now, k.downSleepy);
          else        onB_Double(now, k.downSleepy);
        }
      }
    }
    if (k.releaseAt && (now - k.releaseAt > DOUBLE_TAP_MS)) {
      k.releaseAt = 0;                   // 300ms 内没第二下 → 兑现单击
      if (i == 0) onA_Single(now, k.downSleepy);
      else        onB_Single(now, k.downSleepy);
    }
    k.prev = k.cur;
  }
}

// =================================================================
void loop() {
  unsigned long now = millis();

  // 触摸后的加速窗口：让新表情几乎立刻到位
  gEase = (now < gBoostUntil) ? 0.55f : EASING;

  // ---------- 1. 双键触摸（A=GPIO6 / B=GPIO10） ----------
  pollKey(0, now);
  pollKey(1, now);
  touchLogic(now);

  // 醒来 300ms 后：惊讶 -> 开心
  if (gWakeHappyAt && now >= gWakeHappyAt) {
    gWakeHappyAt = 0;
    setEmotion(HAPPY);
    Serial.println("[A] wake -> HAPPY");
  }

  // A 长按第二段：700ms 后 NEUTRAL -> LOVE，再飘一次心
  if (gLoveAt && now >= gLoveAt) {
    gLoveAt = 0;
    setEmotion(LOVE);
    burstHearts(now);
    gBoostUntil = now + 260;
    Serial.println("[A] long -> LOVE + hearts");
  }

  // ---------- 1.7 震动传感器 ----------
  // 放在触摸和 LOVE 段之后：这样它压上去的就是「最终该显示的那张脸」，
  // 不会被震动表情闪一下再被触摸表情盖回来。
  pollVib(now);
  vibLogic(now);

  // ---------- 1.5 手机通过网页发来的命令 ----------
  // 串口命令：一行一条，和网页共用 execCmd（方便调试）
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (gSerialCmdLen > 0) {
        gSerialCmd[gSerialCmdLen] = 0;
        Serial.printf("[ser] <- %s\n", gSerialCmd);
        execCmd(gSerialCmd, now);
        gSerialCmdLen = 0;
      }
    } else if (gSerialCmdLen < (uint8_t)(sizeof(gSerialCmd) - 1)) {
      gSerialCmd[gSerialCmdLen++] = ch;
    }
  }

  netPoll(now);

  // ---------- 1.9 空闲随机表演：随机 3 个表情轮着来，播完才准进打哈欠 ----------
  if (!gIdleInit || gLastTouch != gIdleSeenTouch) {
    gIdleInit = true;
    gIdleSeenTouch = gLastTouch;         // 有人碰过 → 本轮作废，重新洗牌
    gIdleShowIdx = 0;
    gSleepAt = 0;                        // 作废待定的睡着时刻
    pickIdleFaces();
    gIdleShowAt = now + IDLE_FIRST_MS;
  }
  if (!gDrowsy && !gAsleep && gIdleShowIdx < gIdleTotal && now >= gIdleShowAt) {
    if (gVibStage) {                     // 正在晃：表演先按住，别把 DIZZY 顶掉
      gIdleShowAt = now + 300;
    } else {
      Emotion e = CYCLE[gIdlePick[gIdleShowIdx]];
      setEmotion(e);
      gBoostUntil = now + 200;             // 切换时快一点到位
      Serial.printf("[idle] show %d/%d -> face %d\n",
                    (int)gIdleShowIdx + 1, (int)gIdleTotal, (int)e);
      gIdleShowIdx++;
      gIdleShowAt = now + IDLE_FACE_MS;
    }
  }

  // ---------- 2. 无人理会的计时（困之前先打个哈欠） ----------
  // 只有本轮随机表情全部播完了，才允许进入打哈欠
  if (!gDrowsy && !gAsleep && gIdleShowIdx >= gIdleTotal && (long)(now - gLastTouch) > (long)DROWSY_MS) {
    gDrowsy = true;
    gYawnUntil = now + YAWN_MS;          // 哈欠 0.9s，之后才落成眯缝
    gSleepAt   = now + YAWN_MS + 5000;   // 哈欠结束后再眯 5 秒才真正睡着
    setEmotion(YAWN);
    if (gLoopSleep) gLoopStart = now;    // 1 分钟循环：从"开始犯困"这一刻起算
    Serial.printf("[idle] yawn diff=%lu loop=%d\n", now - gLastTouch, (int)gLoopSleep);
  }
  if (gYawnUntil && now >= gYawnUntil) {
    gYawnUntil = 0;
    setEmotion(SLEEPY);
    gNextZeeAt = now;                    // 犯困立刻冒第一个 z
    Serial.println("[idle] drowsy");
  }
  // 睡着时刻由 gSleepAt 决定：抽到 6/9 个时整段会拉长到 47/69 秒，
  // 但「哈欠 → 眯 5 秒 → 睡着」这段节奏保持不变
  if (!gAsleep && gSleepAt && now >= gSleepAt) {
    gSleepAt   = 0;
    gDrowsy    = true;
    gAsleep    = true;
    gSleepStart = now;
    setEmotion(SLEEPING);
    Serial.printf("[idle] asleep diff=%lu\n", now - gLastTouch);
  }

  // ---------- 2.4 B 双击的「1 分钟循环」：圈走完 → 醒来重来 ----------
  if (gLoopSleep && gLoopStart && (now - gLoopStart >= LOOP_SLEEP_MS)) {
    Serial.println("[loop] 60s up -> wake & restart");
    gLoopStart  = 0;
    gSleepAt    = 0;
    gAsleep     = false;
    gDrowsy     = false;
    gSleepStart = 0;
    for (int i = 0; i < ZEE_MAX; i++) zees[i].on = false;
    gLastTouch  = now;                 // 重新开始 25s 犯困计时
    setEmotion(NEUTRAL);
  }
  // 清醒时把计时器清掉，保证每次入睡都从满圈开始
  if (gLoopStart && !gDrowsy && !gAsleep) { gLoopStart = 0; }

  // ---------- 2.5 打呼噜：犯困小 z，睡着 z 变大变快 ----------
  if ((gAsleep || gDrowsy) && !gYawnUntil && !gVibStage) {
    if (now >= gNextZeeAt) {
      if (gAsleep) {
        // 睡越久 → 尺寸越大、间隔越短（0~40s 内渐进到最快）
        float sleepSec = (now - gSleepStart) / 1000.0f;
        float k = sleepSec / 40.0f;
        if (k > 1.0f) k = 1.0f;
        gNextZeeAt = now + (unsigned long)(1000.0f - 620.0f * k);   // 1000ms → 380ms
        spawnZee(now, 3.4f + 1.6f * k);                              // 半宽 3.4 → 5.0（直接 7x7 起）
      } else {
        gNextZeeAt = now + 1900;                                     // 犯困：慢悠悠
        spawnZee(now, 3.2f);
      }
    }
  } else {
    gNextZeeAt = now + 300;      // 清醒时先把计时推后，免得刚睡着就一串 z
  }

  // ---------- 2.6 惊讶停留由手势逻辑收尾（onSingleTap / onLongPress / onTouchDown 双击） ----------

  // ---------- 3. 自动眨眼 ----------
  if (!gAsleep) {
    if (!isBlinking && (now - lastBlinkTime > blinkInterval)) {
      isBlinking = true;
      lastBlinkTime = now;
      blinkInterval = random(2000, 6000);
    } else if (isBlinking && (now - lastBlinkTime > 150)) {
      isBlinking = false;
      lastBlinkTime = now;
    }
  } else {
    isBlinking = false;
  }

  // ---------- 4. 活跃时的随机小动作（原版状态机） ----------
  if (!gAsleep && !gDrowsy && !gVibStage && (now - lastActionTime > actionInterval)) {
    lastActionTime = now;
    actionInterval = random(1500, 4000);

    int action = random(100);
    if (action < 45) {
      saccade();                       // 眼珠瞟一下
    } else {
      int nextEmo = random(0, 6);      // 0..5，不含 SLEEPING
      if (nextEmo == (int)currentEmotion) nextEmo = (nextEmo + 1) % 6;
      setEmotion((Emotion)nextEmo);
    }
  }

  // ---------- 6. 睡着时的呼吸起伏（越睡越急促） ----------
  // 被晃得显示 INVESTIGATE / ANGRY 时先别呼吸，否则眼睛被压成一条缝，表情看不见
  if (gAsleep && !gVibStage) {
    float sleepSec = (now - gSleepStart) / 1000.0f;
    float freq = 0.0018f + min(0.0016f, sleepSec * 0.00004f);   // 40s 后频率翻倍
    float breath = 3.5f + 1.5f * sinf(now * freq);               // 2.0 ~ 5.0
    leftEye.th  = breath;
    rightEye.th = breath;
    leftEye.tw  = 26;
    rightEye.tw = 26;
  }

  // ---------- 7. 帧限速渲染 ----------
  if (now - lastFrameTime >= FRAME_MS) {
    lastFrameTime = now;

    leftEye.update();
    rightEye.update();
    mouth.update();

    // 打嗝：整脸上弹 3px + 眼睛瞬间瞪大（约 3 帧）
    if (gHicUntil) {
      if (now < gHicUntil) {
        leftEye.y -= 3; rightEye.y -= 3; mouth.y -= 3;
        leftEye.h = min(leftEye.h + 7.0f, 46.0f);
        rightEye.h = min(rightEye.h + 7.0f, 46.0f);
        mouth.h += 4;
      } else {
        gHicUntil = 0;
      }
    }

    u8g2.clearBuffer();
    leftBrow.update();
    rightBrow.update();
    leftBrow.draw();
    rightBrow.draw();
    if (gEyeStyle == 2) {                        // 螺旋眼（DIZZY）
      drawSpiral(40, EYE_CY, 3, 12);
      drawSpiral(88, EYE_CY, 3, 12);
    } else if (gEyeStyle == 3) {                 // X 眼（SNEEZE）
      drawCross(40, EYE_CY, 10);
      drawCross(88, EYE_CY, 10);
    } else if (gEyeStyle == 4) {                 // 心形眼（LOVE）
      drawHeartShape(40, EYE_CY);
      drawHeartShape(88, EYE_CY);
    } else if (gArcEyes) {
      drawArcEye(40, EYE_CY - 2, 13, true);
      drawArcEye(88, EYE_CY - 2, 13, true);
    } else {
      leftEye.draw();
      rightEye.draw();
    }
    mouth.draw();
    drawDeco();
    drawZees(now);
    drawHearts(now);
    drawHourglass(now);
    drawLockBadge(now);
    drawClockBadge(now);
    if (gDumpReq) {
      gDumpReq = false;
      const uint8_t* bp = u8g2.getBufferPtr();
      Serial.printf("[dump]%d:", (int)currentEmotion);
      for (int i = 0; i < 1024; i++) Serial.printf("%02x", bp[i]);
      Serial.println();
    }
    u8g2.sendBuffer();
  }
}

// =================================================================
//  触摸处理
// =================================================================
// （旧手势函数已由文件上方的 onA_* / onB_* 取代）

// =================================================================
//  命令解析：在主循环里跑
// =================================================================
// 命令解析：网页和串口共用同一个入口，永远在主循环里执行
// 手机端（网页 / 串口）改表情前必须先退出犯困/睡眠：
// 否则 loop 里睡眠段每帧强压 th/tw，眼睛会一直是眯缝，z 也继续冒
void remoteWake(unsigned long now) {
  gLastTouch  = now;
  gYawnUntil  = 0;
  gSleepAt    = 0;
  gAsleep     = false;
  gDrowsy     = false;
  gSleepStart = 0;
  for (int i = 0; i < ZEE_MAX; i++) zees[i].on = false;
  Serial.printf("[wake] last=%lu drowsy=%d asleep=%d\n", gLastTouch, (int)gDrowsy, (int)gAsleep);
}

void execCmd(const char* cmd, unsigned long now) {
  if (!cmd || !cmd[0]) return;

  char c = cmd[0];
  int  n = atoi(cmd + 1);

  switch (c) {
    case 'F': case 'f':                     // F0..F21 直接切表情
      if (n >= 0 && n < (int)EMO_COUNT) {
        remoteWake(now);
        setEmotion((Emotion)n);
        // 睡 / 困 要跟状态机联动，否则 z 不会冒出来，而且下一帧就被"无人理会"覆盖
        if (n == SLEEPING) {
          gDrowsy = true; gAsleep = true;
          gSleepStart = now; gNextZeeAt = now;
          gYawnUntil = 0;
        } else if (n == SLEEPY) {
          gDrowsy = true; gAsleep = false;
          gYawnUntil = 0; gNextZeeAt = now;
        } else if (n == YAWN) {
          gDrowsy = true; gAsleep = false;
          gYawnUntil = now + YAWN_MS; gNextZeeAt = now;
        }
        Serial.printf("[cmd] face %d\n", n);
      }
      break;

    case 'T':                               // 远程"摸一下"：惊讶 → 300ms 后开心
      remoteWake(now);
      setEmotion(SURPRISED);
      gWakeHappyAt = now + 300;
      Serial.println("[cmd] remote tap");
      break;

    case 'R':                               // 远程切换锁定（等价 A+B 长按 2 秒）
      gLocked = !gLocked;
      gLockBadgeUntil = now + 1500;
      Serial.printf("[lock] %s (remote)\n", gLocked ? "ON" : "OFF");
      break;

    case 'Q': {                             // 查倒计时内部状态
      long elapsed = gLoopStart ? (long)(now - gLoopStart) : -1;
      long remain  = gLoopStart ? (long)LOOP_SLEEP_MS - elapsed : -1;
      long vibAge  = gVibPulseAt ? (long)(now - gVibPulseAt) : -1;
      Serial.printf("LOOP on=%d start=%lu now=%lu elapsed=%ld remain=%ld drowsy=%d asleep=%d\n",
                    (int)gLoopSleep, gLoopStart, now, elapsed, remain,
                    (int)gDrowsy, (int)gAsleep);
      Serial.printf("VIB  stage=%d level=%d pulseAge=%ld raw=%d\n",
                    (int)gVibStage, (int)gVibLevel, vibAge, (int)gVibRaw);
      break;
    }

    case 'L':                               // 远程长按：呆萌 + 爱心
      remoteWake(now);
      setEmotion(NEUTRAL);
      burstHearts(now);
      Serial.println("[cmd] remote long");
      break;

    case 'D':                               // 远程双击：被逗笑
      remoteWake(now);
      setEmotion(HAPPY);
      burstHeartsUp(now);
      Serial.println("[cmd] remote double");
      break;

    case 'B':                               // B0..B255 调亮度
      gBright = (uint8_t)constrain(n, 0, 255); u8g2.setContrast(gBright);
      Serial.printf("[cmd] contrast %d\n", n);
      break;

    case 'K':                               // 切换「1 分钟循环」（等价 B 双击，但不受锁死影响）
      gLoopSleep = !gLoopSleep;
      gLoopStart = 0;
      gClockBadgeOn = gLoopSleep;
      gClockBadgeUntil = now + 1500;
      Serial.printf("[cmd] loopSleep %s\n", gLoopSleep ? "ON" : "OFF");
      break;

    // ---- 手势模拟（走真实的回调，用来验证锁死逻辑）----
    case 'Y': onA_Single(now, isSleepyState());  Serial.println("[sim] A single"); break;
    case 'U': onA_Long  (now, isSleepyState());  Serial.println("[sim] A long");   break;
    case 'I': onA_Double(now, isSleepyState());  Serial.println("[sim] A double"); break;
    case 'O': onB_Single(now, isSleepyState());  Serial.println("[sim] B single"); break;
    case 'J': onB_Double(now, isSleepyState());  Serial.println("[sim] B double"); break;
    case 'N': onB_Long  (now);                   Serial.println("[sim] B long");   break;

    case 'V':                               // 模拟一次震动脉冲（不用真晃）
      gVibPulseAt = now;
      Serial.printf("[cmd] vibrate pulse (stage=%d)\n", (int)gVibStage);
      break;

    case 'P':                               // 调试：dump 当前帧缓冲（hex）
      gDumpReq = true;
      Serial.println("[cmd] dump frame");
      break;

    case 'S': {                             // 查状态
      char buf[64];
      snprintf(buf, sizeof(buf), "ST face=%d up=%lus touch=%lus\n",
               (int)currentEmotion, now / 1000, gLastTouch / 1000);
      netNotify(buf);
      break;
    }
  }
}

// =================================================================
//  打呼噜的 z
// =================================================================
void spawnZee(unsigned long now, float s0) {
  for (int i = 0; i < ZEE_MAX; i++) {
    if (zees[i].on) continue;
    zees[i].on   = true;
    zees[i].x    = 99 + random(0, 7);
    zees[i].y    = 16 + random(0, 5);
    zees[i].vx   = 0.16f + random(0, 10) * 0.02f;    // 往右上飘
    zees[i].vy   = -0.26f - random(0, 10) * 0.02f;
    zees[i].s0   = s0 + random(0, 5) * 0.12f;
    zees[i].born = now;
    zees[i].life = 1500 + random(0, 400);
    return;
  }
}

// 位图 Z：按当前尺寸在三档里切换，保证永远是干净的 Z 形（不会糊成竖杠）
void drawZee(float cx, float cy, float s) {
  int x = (int)roundf(cx);
  int y = (int)roundf(cy);
  if (s < 2.6f)      u8g2.drawXBMP(x - 2, y - 2, 5, 5, ZEE5);
  else if (s < 3.8f) u8g2.drawXBMP(x - 3, y - 3, 7, 7, ZEE7);
  else               u8g2.drawXBMP(x - 4, y - 4, 9, 9, ZEE9);
}

void drawZees(unsigned long now) {
  for (int i = 0; i < ZEE_MAX; i++) {
    if (!zees[i].on) continue;
    unsigned long age = now - zees[i].born;
    if (age > zees[i].life) { zees[i].on = false; continue; }

    float t = (float)age / (float)zees[i].life;
    float frames = age / (float)FRAME_MS;
    float s = zees[i].s0 * (0.72f + 0.62f * t);          // 一路飘一路变大
    float x = zees[i].x + zees[i].vx * frames;
    float y = zees[i].y + zees[i].vy * frames;

    if (x > SCREEN_WIDTH - 2 || y < 1) continue;         // 飘出屏幕就不再画
    drawZee(x, y, s);
  }
}

// =================================================================
//  触摸冒爱心
// =================================================================
void burstHearts(unsigned long now) {
  for (int side = 0; side < 2; side++) {
    for (int k = 0; k < 3; k++) {
      for (int i = 0; i < HEART_MAX; i++) {
        if (hearts[i].on) continue;
        hearts[i].on   = true;
        hearts[i].x    = side ? (103 + k * 7) : (25 - k * 7);
        hearts[i].y    = 47 - k * 6;
        hearts[i].vx   = side ? 0.14f : -0.14f;          // 往外飘
        hearts[i].vy   = -0.40f - k * 0.07f;             // 往上飘
        hearts[i].big  = (k == 1) ? 1 : 0;
        hearts[i].born = now + k * 90;                    // 错峰冒
        hearts[i].life = 900 + k * 90;
        break;
      }
    }
  }
}

// 双击被逗笑：6 颗心从屏幕底部一起往上冲
void burstHeartsUp(unsigned long now) {
  for (int k = 0; k < 6; k++) {
    for (int i = 0; i < HEART_MAX; i++) {
      if (hearts[i].on) continue;
      hearts[i].on   = true;
      hearts[i].x    = 34 + k * 12;                   // 34..94 横向铺开
      hearts[i].y    = 62;                             // 从底边起飞
      hearts[i].vx   = (k - 2.5f) * 0.06f;             // 左右微微散开
      hearts[i].vy   = -0.80f - (k % 3) * 0.10f;       // 一路往上
      hearts[i].big  = (k % 2);
      hearts[i].born = now + k * 55;                    // 错峰
      hearts[i].life = 1000;
      break;
    }
  }
}

// -----------------------------------------------------------------
//  1 分钟循环的沙漏倒计时（只在犯困/睡着 + 循环开启时画）
//
//  左上角 13x11，外面再套 1px 边框（占 15x13，紧贴屏幕左上角）
//  优先级最低：锁标 / 闹钟亮着的时候整块让位，等它们 1.5 秒走完再回来
//  上腔每 15 秒掉一行沙，沙整体下沉（顶部先空）
//  颈口每 250ms 一格往下滴，落到沙堆就停
// -----------------------------------------------------------------
void drawHourglass(unsigned long now) {
  if (!gLoopSleep || !gLoopStart || !(gDrowsy || gAsleep)) return;
  // 锁标 / 闹钟优先：它们显示期间沙漏完全不画
  if (gLockBadgeUntil  && now < gLockBadgeUntil)  return;
  if (gClockBadgeUntil && now < gClockBadgeUntil) return;

  long left = (long)LOOP_SLEEP_MS - (long)(now - gLoopStart);
  if (left < 0) left = 0;
  long elapsed = (long)LOOP_SLEEP_MS - left;

  const int X0 = 1, Y0 = 1;           // 最左上角
  const int W  = 13, H = 11;

  // 1px 外框
  u8g2.drawFrame(X0 - 1, Y0 - 1, W + 2, H + 2);

  // 顶框 / 底框
  for (int x = 0; x < W; x++) {
    u8g2.drawPixel(X0 + x, Y0);
    u8g2.drawPixel(X0 + x, Y0 + 10);
  }

  // 上腔还剩几行沙：60 秒 4 档，每 15 秒掉 1 行
  int keep = 4 - (int)(elapsed / 15000);
  if (keep < 0) keep = 0;
  if (keep > 4) keep = 4;

  // 上腔 4 行（y+1..y+4），梯形宽度 11/9/6/3
  static const uint8_t upW[4] = {11, 9, 6, 3};
  for (int r = 0; r < 4; r++) {
    if (r < 4 - keep) continue;                       // 顶部先空
    int w = upW[r];
    int sx = X0 + 6 - w / 2;
    for (int k = 0; k < w; k++) u8g2.drawPixel(sx + k, Y0 + 1 + r);
  }

  // 颈
  u8g2.drawPixel(X0 + 6, Y0 + 5);

  // 下腔 4 行（y+6..y+9），梯形宽度 3/6/9/11，从底部往上堆
  static const uint8_t dnW[4] = {3, 6, 9, 11};
  for (int r = 0; r < 4; r++) {
    if (r < keep) continue;                           // 沙不够，顶部空着
    int w = dnW[r];
    int sx = X0 + 6 - w / 2;
    for (int k = 0; k < w; k++) u8g2.drawPixel(sx + k, Y0 + 6 + r);
  }

  // 颈口滴落：每 250ms 走一格，落到沙堆顶部就消失
  int lowerTop = Y0 + 6 + keep;
  int p = (int)((now / 250) % 4);
  for (int k = 0; k < 4; k++) {
    int yy = Y0 + 6 + k;
    if (yy >= lowerTop) break;
    if (k == p) { u8g2.drawPixel(X0 + 6, yy); break; }
  }
}

void drawHearts(unsigned long now) {
  for (int i = 0; i < HEART_MAX; i++) {
    if (!hearts[i].on) continue;
    if (now < hearts[i].born) continue;                   // 还没到登场时间
    unsigned long age = now - hearts[i].born;
    if (age > hearts[i].life) { hearts[i].on = false; continue; }

    float frames = age / (float)FRAME_MS;
    float x = hearts[i].x + hearts[i].vx * frames;
    float y = hearts[i].y + hearts[i].vy * frames;

    if (y < -6) { hearts[i].on = false; continue; }
    if (hearts[i].big) u8g2.drawXBMP((int)roundf(x), (int)roundf(y), 7, 6, HEART7);
    else               u8g2.drawXBMP((int)roundf(x), (int)roundf(y), 5, 5, HEART5);
  }
}

// =================================================================
//  表情装饰件（泪滴 / 问号 / 音符 / 舌头 / 腮红 / 小爱心）
// =================================================================
void drawTear(int cx, int cy) {
  u8g2.drawTriangle(cx, cy - 4, cx - 3, cy + 3, cx + 3, cy + 3);
  u8g2.drawDisc(cx, cy + 3, 3);
}

static const unsigned char QMARK5[7] PROGMEM = {
  0x70, 0x88, 0x08, 0x10, 0x20, 0x00, 0x20
};

void drawQuestion(int cx, int cy) {
  u8g2.drawXBMP(cx, cy, 5, 7, QMARK5);
}

void drawNote(int cx, int cy) {
  u8g2.drawLine(cx + 5, cy - 9, cx + 5, cy + 1);
  u8g2.drawLine(cx + 6, cy - 9, cx + 6, cy + 1);
  u8g2.drawLine(cx + 5, cy - 9, cx + 11, cy - 11);
  u8g2.drawDisc(cx + 2, cy + 2, 3);
}

void drawTongue(int cx, int cy) {
  u8g2.drawRBox(cx - 5, cy, 11, 9, 4);
}

void drawBlush(int cx, int cy) {
  u8g2.drawLine(cx - 5, cy, cx - 2, cy + 3);
  u8g2.drawLine(cx,     cy, cx + 3, cy + 3);
  u8g2.drawLine(cx + 5, cy, cx + 8, cy + 3);
}

void drawSmallHeart(int cx, int cy) {
  u8g2.drawXBMP(cx - 3, cy - 3, 7, 6, HEART7);
}

// 13x12 实心心形（LOVE 当眼睛用）
static const uint8_t HEART13[] PROGMEM = {
  0x38,0xe0,0x7d,0xf0,0xfd,0xf8,0xff,0xf8,0xff,0xf8,0x7f,0xf0,
  0x3f,0xe0,0x1f,0xc0,0x0f,0x80,0x07,0x00,0x02,0x00,0x02,0x00
};

void drawHeartShape(int cx, int cy) {
  // 用几何拼心形：两个圆瓣 + 一个下三角。比 13x12 位图在 SSD1306 上更可靠，
  // 也不会因为 XBMP 的宽度不是 8 的倍数而错行。
  u8g2.drawDisc(cx - 3, cy - 2, 4);
  u8g2.drawDisc(cx + 3, cy - 2, 4);
  u8g2.drawTriangle(cx - 7, cy - 1, cx + 7, cy - 1, cx, cy + 7);
  // 挖出中间那条缝，让两个圆瓣分开成心形
  u8g2.setDrawColor(0);
  u8g2.drawTriangle(cx - 1, cy - 8, cx + 1, cy - 8, cx, cy - 2);
  u8g2.setDrawColor(1);
}

// 锁定状态切换后，左上角显示 1.5 秒的小标记：
//   锁上 → 梁和体连着；解锁 → 梁抬起右移，和体之间留一道缝。平时不占屏幕。
void drawLockBadge(unsigned long now) {
  if (!gLockBadgeUntil || now >= gLockBadgeUntil) return;
  const int x = 2, y = 3;

  if (gLocked) {
    // 闭合：拱形梁压在锁体正上方
    u8g2.drawPixel(x + 1, y);      u8g2.drawPixel(x + 2, y);
    u8g2.drawPixel(x + 3, y);      u8g2.drawPixel(x + 4, y);
    u8g2.drawPixel(x,     y + 1);  u8g2.drawPixel(x + 5, y + 1);
    u8g2.drawPixel(x,     y + 2);  u8g2.drawPixel(x + 5, y + 2);
    u8g2.drawBox(x, y + 3, 6, 4);                 // 锁体
    u8g2.setDrawColor(0);
    u8g2.drawPixel(x + 2, y + 5);                 // 锁孔
    u8g2.setDrawColor(1);
  } else {
    // 打开：梁抬起并右移，锁体下移一行，中间露出缝
    u8g2.drawPixel(x + 2, y);      u8g2.drawPixel(x + 3, y);
    u8g2.drawPixel(x + 4, y);      u8g2.drawPixel(x + 5, y);
    u8g2.drawPixel(x + 1, y + 1);  u8g2.drawPixel(x + 6, y + 1);
    u8g2.drawPixel(x + 1, y + 2);  u8g2.drawPixel(x + 6, y + 2);
    u8g2.drawBox(x, y + 4, 6, 3);                 // 锁体（矮一行，位置下移）
    u8g2.setDrawColor(0);
    u8g2.drawPixel(x + 2, y + 5);
    u8g2.setDrawColor(1);
  }
}

// 双击 B 切换倒计时后，左上角显示 1.5 秒的小闹钟：
//   开 → 只画闹钟；关 → 闹钟上压一道斜杠。平时不占屏幕。
void drawClockBadge(unsigned long now) {
  if (!gClockBadgeUntil || now >= gClockBadgeUntil) return;
  const int cx = 6, cy = 7, r = 4;

  u8g2.drawCircle(cx, cy, r);          // 表盘
  u8g2.drawPixel(cx - 3, cy - 4);      // 左铃（圆外两侧）
  u8g2.drawPixel(cx + 3, cy - 4);      // 右铃
  u8g2.drawPixel(cx, cy - 2);          // 分针（指向 12）
  u8g2.drawPixel(cx, cy - 1);
  u8g2.drawPixel(cx + 1, cy);          // 时针（指向 3）
  u8g2.drawPixel(cx + 2, cy);

  if (!gClockBadgeOn) u8g2.drawLine(2, 2, 11, 12);   // 关掉：一道斜杠
}

// 各表情的挂件装饰：只在对应情绪下画
void drawDeco() {
  switch (currentEmotion) {
    case CRY:
      drawTear(30, 44);
      drawTear(98, 48);
      break;
    case SHY:
      drawBlush(16, 44);
      drawBlush(112, 44);
      drawSmallHeart(64, 12);
      break;
    case THINK:
      drawQuestion(108, 12);
      break;
    case YUMMY:
      drawTongue(64, 54);
      break;
    case MOCK:
      drawTongue(72, 54);
      break;
    case MUSIC:
      drawNote(10, 26);
      drawNote(100, 38);
      break;
    case SNEEZE:                              // 两侧飞沫
      u8g2.drawLine(12, 42, 13, 46);
      u8g2.drawLine(16, 44, 17, 48);
      u8g2.drawLine(20, 42, 21, 46);
      u8g2.drawLine(106, 46, 107, 50);
      u8g2.drawLine(110, 44, 111, 48);
      u8g2.drawLine(114, 46, 115, 50);
      break;
    case INVESTIGATE:                         // 右上问号
      drawQuestion(106, 8);
      break;
    default:
      break;
  }
}

// =================================================================
//  情绪表（数值全部沿用原版，128x64 上已调好）
// =================================================================
void setEmotion(Emotion emo) {
  currentEmotion = emo;

  float cy = EYE_CY;
  float lx = (SCREEN_WIDTH / 2) - 24;
  float rx = (SCREEN_WIDTH / 2) + 24;
  float mx = SCREEN_WIDTH / 2;
  float my = MOUTH_CY;

  // 换情绪时把扫视偏移归零
  leftEye.ox = leftEye.oy = 0;
  rightEye.ox = rightEye.oy = 0;
  mouth.ox = mouth.oy = 0;
  gArcEyes = false;            // 默认胶囊眼，只有月牙眼的情绪会打开
  gEyeStyle = 0;               // 默认胶囊眼，只有螺旋/叉/心的情绪会改

  switch (emo) {
    case NEUTRAL:
      setEyeTargets(lx, cy, 24, 34, rx, cy, 24, 34);
      setMouthTargets(mx, my, 16, 4);
      setBrows(lx, 10, 11, 0, rx, 10, 11, 0, true);
      break;
    case HAPPY:
      setEyeTargets(lx, cy - 4, 28, 18, rx, cy - 4, 28, 18);
      setMouthTargets(mx, my, 30, 9);          // 咧嘴笑
      setBrows(lx, 8, 12, -2, rx, 8, 12, -2, true);
      break;
    case SAD:
      setEyeTargets(lx + 4, cy + 8, 20, 22, rx - 4, cy + 8, 20, 22);
      setMouthTargets(mx, my + 2, 12, 4);      // 小小的、往下
      setBrows(lx, 10, 12, -3, rx, 10, 12, -3, true);
      break;
    case SURPRISED:
      setEyeTargets(lx, cy - 4, 18, 32, rx, cy - 4, 18, 32);
      setMouthTargets(mx, my + 1, 10, 11);     // 圆嘴 o
      setBrows(lx, 4, 12, -3, rx, 4, 12, -3, true);
      break;
    case SLEEPY:
      setEyeTargets(lx, cy + 12, 26, 8, rx, cy + 12, 26, 8);
      setMouthTargets(mx, my + 1, 8, 3);
      setBrows(lx, 0, 12, 0, rx, 0, 12, 0, false);
      break;
    case ANGRY:
      setEyeTargets(lx + 8, cy + 4, 22, 20, rx - 8, cy + 4, 22, 20);
      setMouthTargets(mx, my, 22, 3);          // 一条横线
      setBrows(lx, 14, 12, 4, rx, 14, 12, 4, true);
      break;
    case SLEEPING:
      setEyeTargets(lx, cy + 14, 26, 3, rx, cy + 14, 26, 3);
      setMouthTargets(mx, my + 1, 8, 2);
      setBrows(lx, 0, 12, 0, rx, 0, 12, 0, false);
      break;
    case YAWN:
      // 打哈欠：眼眯一半、嘴张成 16x16 圆、整张脸往上抬 2px
      setEyeTargets(lx, cy + 6, 26, 18, rx, cy + 6, 26, 18);
      setMouthTargets(mx, my + 2, 16, 16);
      leftEye.oy = rightEye.oy = -2;
      mouth.oy = -2;
      setBrows(lx, 0, 12, 0, rx, 0, 12, 0, false);
      break;

    // ---- 以下 8 个来自 emote-assets 的资源名 ----
    case LAUGH:                                // laugh / snigger：月牙眼 + 大张嘴
      gArcEyes = true;
      setEyeTargets(lx, cy - 2, 26, 14, rx, cy - 2, 26, 14);
      setMouthTargets(mx, my - 4, 36, 16);
      setBrows(lx, 8, 13, -3, rx, 8, 13, -3, true);
      break;
    case CRY:                                  // cry：八字眉 + 两侧泪滴
      setEyeTargets(lx + 3, cy + 6, 20, 24, rx - 3, cy + 6, 20, 24);
      setMouthTargets(mx, my + 3, 14, 7);
      setBrows(lx, 9, 12, -4, rx, 9, 12, -4, true);
      break;
    case SHY:                                  // shy：腮红 + 头顶小爱心
      setEyeTargets(lx + 2, cy + 2, 22, 24, rx - 2, cy + 2, 22, 24);
      setMouthTargets(mx, my, 14, 4);
      setBrows(lx, 11, 12, -1, rx, 11, 12, -1, true);
      break;
    case THINK:                                // ponder / question / investigate
      setEyeTargets(lx, cy - 10, 20, 22, rx, cy - 10, 20, 22);
      setMouthTargets(mx, my, 10, 3);
      setBrows(lx, 2, 12, -4, rx, 2, 12, -4, true);
      break;
    case YUMMY:                                // yummy：月牙眼 + 吐舌
      gArcEyes = true;
      setEyeTargets(lx, cy - 2, 26, 14, rx, cy - 2, 26, 14);
      setMouthTargets(mx, my - 2, 22, 11);
      setBrows(lx, 8, 13, -3, rx, 8, 13, -3, true);
      break;
    case MOCK:                                 // mock：一高一低眉 + 斜舌
      setEyeTargets(lx, cy - 4, 24, 16, rx - 6, cy - 2, 20, 12);
      setMouthTargets(mx, my, 18, 6);
      setBrows(lx, 9, 12, -2, rx, 4, 12, -4, true);
      break;
    case MUSIC:                                // music：闭眼 + 音符
      setEyeTargets(lx, cy + 10, 26, 4, rx, cy + 10, 26, 4);
      setMouthTargets(mx, my, 18, 6);
      setBrows(lx, 0, 12, 0, rx, 0, 12, 0, false);
      break;
    case CONFIDENT:                            // confident / badminton：挑眉
      setEyeTargets(lx + 4, cy - 2, 22, 16, rx - 4, cy - 2, 22, 16);
      setMouthTargets(mx, my, 22, 5);
      setBrows(lx, 8, 12, 1, rx, 3, 12, -4, true);
      break;

    case PANIC:                                // 慌：一大一小眼 + 圆嘴 + 眉两头翘
      setEyeTargets(lx, cy - 4, 22, 26, rx, cy - 2, 18, 32);
      setMouthTargets(mx, my - 3, 18, 12);
      setBrows(lx, 4, 12, 3, rx, 5, 12, 5, true);
      break;

    case DIZZY:                                // 晕：螺旋眼 + 小横嘴
      setEyeTargets(lx, cy, 24, 24, rx, cy, 24, 24);
      setMouthTargets(mx, my, 16, 4);
      setBrows(lx, 0, 12, 0, rx, 0, 12, 0, false);
      gEyeStyle = 2;
      break;

    case LOVE:                                 // 爱心眼 + 笑弧
      setEyeTargets(lx, cy, 22, 22, rx, cy, 22, 22);
      setMouthTargets(mx, my + 2, 20, 4);
      setBrows(lx, 0, 12, 0, rx, 0, 12, 0, false);
      gEyeStyle = 4;
      break;

    case SNEEZE:                               // 喷嚏：X 眼 + 张大嘴
      setEyeTargets(lx, cy, 22, 22, rx, cy, 22, 22);
      setMouthTargets(mx, my - 4, 16, 13);
      setBrows(lx, 0, 12, 0, rx, 0, 12, 0, false);
      gEyeStyle = 3;
      break;

    case HELLO:                                // 打招呼：单眼眨 + 微笑
      setEyeTargets(lx, cy, 26, 28, rx, cy, 20, 6);
      setMouthTargets(mx, my - 4, 20, 9);
      setBrows(lx, 8, 12, -2, rx, 10, 12, 2, true);
      break;

    case INVESTIGATE:                          // 打量：眯眼 + 一高一低眉 + 问号
      setEyeTargets(lx, cy + 2, 20, 10, rx, cy + 2, 20, 10);
      setMouthTargets(mx, my - 1, 10, 3);
      setBrows(lx, 14, 12, 5, rx, 5, 12, -6, true);
      break;
  }

  // 打嗝彩蛋：每次换表情 1/300 抽一次（睡着/哈欠时不抽）
  if (emo != SLEEPING && emo != YAWN && random(300) == 0) {
    gHicUntil = millis() + HIC_MS;
    Serial.println("[egg] hic");
  }

  // 有在线通道（蓝牙/网页）就记一笔当前表情
  {
    char ev[16];
    snprintf(ev, sizeof(ev), "EV %d", (int)emo);
    netNotify(ev);
  }
}

// 扫视：设置而非累加偏移，嘴跟一半强度（原版逻辑）
void saccade() {
  float offsetX = random(-12, 13);
  float offsetY = random(-8, 9);

  leftEye.ox = rightEye.ox = offsetX;
  leftEye.oy = rightEye.oy = offsetY;
  mouth.ox = offsetX * 0.5f;
  mouth.oy = 0;
}

void setEyeTargets(float lx, float ly, float lw, float lh,
                   float rx, float ry, float rw, float rh) {
  leftEye.tx = lx;
  leftEye.ty = ly;
  leftEye.tw = lw;
  leftEye.th = lh;
  rightEye.tx = rx;
  rightEye.ty = ry;
  rightEye.tw = rw;
  rightEye.th = rh;
}

void setMouthTargets(float x, float y, float w, float h) {
  mouth.tx = x;
  mouth.ty = y;
  mouth.tw = w;
  mouth.th = h;
}
