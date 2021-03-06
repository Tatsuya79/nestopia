#include "libretro.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <sstream>
#include <fstream>

#include "../source/core/api/NstApiMachine.hpp"
#include "../source/core/api/NstApiEmulator.hpp"
#include "../source/core/api/NstApiVideo.hpp"
#include "../source/core/api/NstApiCheats.hpp"
#include "../source/core/api/NstApiSound.hpp"
#include "../source/core/api/NstApiInput.hpp"
#include "../source/core/api/NstApiCartridge.hpp"
#include "../source/core/api/NstApiUser.hpp"
#include "../source/core/api/NstApiFds.hpp"

#define NST_VERSION "1.48-WIP"

#ifdef _WIN32
#define snprintf _snprintf
#endif

#define NES_NTSC_PAR ((Api::Video::Output::WIDTH - (overscan_h ? 16 : 0)) * (8.0 / 7.0)) / (Api::Video::Output::HEIGHT - (overscan_v ? 16 : 0))
#define NES_PAL_PAR ((Api::Video::Output::WIDTH - (overscan_h ? 16 : 0)) * (2950000.0 / 2128137.0)) / (Api::Video::Output::HEIGHT - (overscan_v ? 16 : 0))
#define NES_4_3_DAR (4.0 / 3.0);

using namespace Nes;

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

#ifdef _3DS
extern "C" void* linearMemAlign(size_t size, size_t alignment);
extern "C" void linearFree(void* mem);
#endif
static uint32_t* video_buffer = NULL;

static int16_t audio_buffer[(44100 / 50)];
static int16_t audio_stereo_buffer[2 * (44100 / 50)];
static Api::Emulator emulator;
static Api::Machine *machine;
static Api::Fds *fds;
static char g_basename[256];
static char g_rom_dir[256];
static char *g_save_dir;
static unsigned blargg_ntsc;
static bool fds_auto_insert;
static bool overscan_v;
static bool overscan_h;
static unsigned aspect_ratio_mode;

int16_t video_width = Api::Video::Output::WIDTH;
size_t pitch;

static Api::Video::Output *video;
static Api::Sound::Output *audio;
static Api::Input::Controllers *input;
static Api::Machine::FavoredSystem favsystem;

static void *sram;
static unsigned long sram_size;
static bool is_pal;
static bool dbpresent;
static unsigned char *custpal[64*3];

static const byte nescap_palette[64][3] =
{
   {0x64,0x63,0x65}, {0x00,0x15,0x80}, {0x1d,0x00,0x90}, {0x38,0x00,0x82},
   {0x56,0x00,0x5d}, {0x5a,0x00,0x1a}, {0x4f,0x09,0x00}, {0x38,0x1b,0x00},
   {0x1e,0x31,0x00}, {0x00,0x3d,0x00}, {0x00,0x41,0x00}, {0x00,0x3a,0x1b},
   {0x00,0x2f,0x55}, {0x00,0x00,0x00}, {0x00,0x00,0x00}, {0x00,0x00,0x00},
   {0xaf,0xad,0xaf}, {0x16,0x4b,0xca}, {0x47,0x2a,0xe7}, {0x6b,0x1b,0xdb},
   {0x96,0x17,0xb0}, {0x9f,0x18,0x5b}, {0x96,0x30,0x01}, {0x7b,0x48,0x00},
   {0x5a,0x66,0x00}, {0x23,0x78,0x00}, {0x01,0x7f,0x00}, {0x00,0x78,0x3d},
   {0x00,0x6c,0x8c}, {0x00,0x00,0x00}, {0x00,0x00,0x00}, {0x00,0x00,0x00},
   {0xff,0xff,0xff}, {0x60,0xa6,0xff}, {0x8f,0x84,0xff}, {0xb4,0x73,0xff},
   {0xe2,0x6c,0xff}, {0xf2,0x68,0xc3}, {0xef,0x7e,0x61}, {0xd8,0x95,0x27},
   {0xba,0xb3,0x07}, {0x81,0xc8,0x07}, {0x57,0xd4,0x3d}, {0x47,0xcf,0x7e},
   {0x4b,0xc5,0xcd}, {0x4c,0x4b,0x4d}, {0x00,0x00,0x00}, {0x00,0x00,0x00},
   {0xff,0xff,0xff}, {0xc2,0xe0,0xff}, {0xd5,0xd2,0xff}, {0xe3,0xcb,0xff},
   {0xf7,0xc8,0xff}, {0xfe,0xc6,0xee}, {0xfe,0xce,0xc6}, {0xf6,0xd7,0xae},
   {0xe9,0xe4,0x9f}, {0xd3,0xed,0x9d}, {0xc0,0xf2,0xb2}, {0xb9,0xf1,0xcc},
   {0xba,0xed,0xed}, {0xba,0xb9,0xbb}, {0x00,0x00,0x00}, {0x00,0x00,0x00}
};
static const byte unsaturated_v7_palette[64][3] =
{
   {0x67,0x67,0x67}, {0x00,0x1f,0x8e}, {0x23,0x06,0x9e}, {0x40,0x00,0x8e},
   {0x60,0x00,0x67}, {0x67,0x00,0x1c}, {0x5b,0x10,0x00}, {0x43,0x25,0x00},
   {0x31,0x34,0x00}, {0x07,0x48,0x00}, {0x00,0x4f,0x00}, {0x00,0x46,0x22},
   {0x00,0x3a,0x61}, {0x00,0x00,0x00}, {0x00,0x00,0x00}, {0x00,0x00,0x00},
   {0xb3,0xb3,0xb3}, {0x20,0x5a,0xdf}, {0x51,0x38,0xfb}, {0x7a,0x27,0xee},
   {0xa5,0x20,0xc2}, {0xb0,0x22,0x6b}, {0xad,0x37,0x02}, {0x8d,0x56,0x00},
   {0x6e,0x70,0x00}, {0x2e,0x8a,0x00}, {0x06,0x92,0x00}, {0x00,0x8a,0x47},
   {0x03,0x7b,0x9b}, {0x10,0x10,0x10}, {0x00,0x00,0x00}, {0x00,0x00,0x00},
   {0xff,0xff,0xff}, {0x62,0xae,0xff}, {0x91,0x8b,0xff}, {0xbc,0x78,0xff},
   {0xe9,0x6e,0xff}, {0xfc,0x6c,0xcd}, {0xfa,0x82,0x67}, {0xe2,0x9b,0x26},
   {0xc0,0xb9,0x01}, {0x84,0xd2,0x00}, {0x58,0xde,0x38}, {0x46,0xd9,0x7d},
   {0x49,0xce,0xd2}, {0x49,0x49,0x49}, {0x00,0x00,0x00}, {0x00,0x00,0x00},
   {0xff,0xff,0xff}, {0xc1,0xe3,0xff}, {0xd5,0xd4,0xff}, {0xe7,0xcc,0xff},
   {0xfb,0xc9,0xff}, {0xff,0xc7,0xf0}, {0xff,0xd0,0xc5}, {0xf8,0xda,0xaa},
   {0xeb,0xe6,0x9a}, {0xd1,0xf1,0x9a}, {0xbe,0xf7,0xaf}, {0xb6,0xf4,0xcd},
   {0xb7,0xf0,0xef}, {0xb2,0xb2,0xb2}, {0x00,0x00,0x00}, {0x00,0x00,0x00}
};

static const byte pal_palette[64][3] =
{
  {0x80,0x80,0x80}, {0x00,0x00,0xBA}, {0x37,0x00,0xBF}, {0x84,0x00,0xA6},
  {0xBB,0x00,0x6A}, {0xB7,0x00,0x1E}, {0xB3,0x00,0x00}, {0x91,0x26,0x00},
  {0x7B,0x2B,0x00}, {0x00,0x3E,0x00}, {0x00,0x48,0x0D}, {0x00,0x3C,0x22},
  {0x00,0x2F,0x66}, {0x00,0x00,0x00}, {0x05,0x05,0x05}, {0x05,0x05,0x05},
  {0xC8,0xC8,0xC8}, {0x00,0x59,0xFF}, {0x44,0x3C,0xFF}, {0xB7,0x33,0xCC},
  {0xFE,0x33,0xAA}, {0xFE,0x37,0x5E}, {0xFE,0x37,0x1A}, {0xD5,0x4B,0x00},
  {0xC4,0x62,0x00}, {0x3C,0x7B,0x00}, {0x1D,0x84,0x15}, {0x00,0x95,0x66},
  {0x00,0x84,0xC4}, {0x11,0x11,0x11}, {0x09,0x09,0x09}, {0x09,0x09,0x09},
  {0xFE,0xFE,0xFE}, {0x00,0x95,0xFF}, {0x6F,0x84,0xFF}, {0xD5,0x6F,0xFF},
  {0xFE,0x77,0xCC}, {0xFE,0x6F,0x99}, {0xFE,0x7B,0x59}, {0xFE,0x91,0x5F},
  {0xFE,0xA2,0x33}, {0xA6,0xBF,0x00}, {0x51,0xD9,0x6A}, {0x4D,0xD5,0xAE},
  {0x00,0xD9,0xFF}, {0x66,0x66,0x66}, {0x0D,0x0D,0x0D}, {0x0D,0x0D,0x0D},
  {0xFE,0xFE,0xFE}, {0x84,0xBF,0xFF}, {0xBB,0xBB,0xFF}, {0xD0,0xBB,0xFF},
  {0xFE,0xBF,0xEA}, {0xFE,0xBF,0xCC}, {0xFE,0xC4,0xB7}, {0xFE,0xCC,0xAE},
  {0xFE,0xD9,0xA2}, {0xCC,0xE1,0x99}, {0xAE,0xEE,0xB7}, {0xAA,0xF8,0xEE},
  {0xB3,0xEE,0xFF}, {0xDD,0xDD,0xDD}, {0x11,0x11,0x11}, {0x11,0x11,0x11}
};

int crossx = 0;
int crossy = 0;

void draw_crosshair(int x, int y)
{
   uint32_t w = 0xFFFFFFFF;
   uint32_t b = 0x00000000;
   
   for(int i = -3; i < 4; i++) {
      video_buffer[256 * y + x + i] = b;
      video_buffer[256 * (y + i) + x] = b;
   }
   
   for(int i = -2; i < 3; i += 2) {
      video_buffer[256 * y + x + i] = w;
      video_buffer[256 * (y + i) + x] = w;
   }
}

static void NST_CALLBACK file_io_callback(void*, Api::User::File &file)
{
   const void *addr;
   unsigned long addr_size;
   char slash;

#ifdef _WIN32
   slash = '\\';
#else
   slash = '/';
#endif

   switch (file.GetAction())
   {
      case Api::User::File::LOAD_BATTERY:
      case Api::User::File::LOAD_EEPROM:
      case Api::User::File::LOAD_TAPE:
      case Api::User::File::LOAD_TURBOFILE:
         file.GetRawStorage(sram, sram_size);
         break;

      case Api::User::File::SAVE_BATTERY:
      case Api::User::File::SAVE_EEPROM:
      case Api::User::File::SAVE_TAPE:
      case Api::User::File::SAVE_TURBOFILE:
         file.GetContent(addr, addr_size);
         if (addr != sram || sram_size != addr_size)
            if (log_cb)
               log_cb(RETRO_LOG_INFO, "[Nestopia]: SRAM changed place in RAM!\n");
         break;
      case Api::User::File::LOAD_FDS:
         {
            char base[256];
            snprintf(base, sizeof(base), "%s%c%s.sav", g_save_dir, slash, g_basename);
            if (log_cb)
               log_cb(RETRO_LOG_INFO, "Want to load FDS sav from: %s\n", base);
            std::ifstream in_tmp(base,std::ifstream::in|std::ifstream::binary);

            if (!in_tmp.is_open())
               return;

            file.SetPatchContent(in_tmp);
         }
         break;
      case Api::User::File::SAVE_FDS:
         {
            char base[256];
            snprintf(base, sizeof(base), "%s%c%s.sav", g_save_dir, slash, g_basename);
            if (log_cb)
               log_cb(RETRO_LOG_INFO, "Want to save FDS sav to: %s\n", base);
            std::ofstream out_tmp(base,std::ifstream::out|std::ifstream::binary);

            if (out_tmp.is_open())
               file.GetPatchContent(Api::User::File::PATCH_UPS, out_tmp);
         }
         break;
      default:
         break;
   }
}

static void check_system_specs(void)
{
   unsigned level = 6;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_init(void)
{
   struct retro_log_callback log;
#ifdef _3DS
   video_buffer = (uint32_t*)linearMemAlign(Api::Video::Output::NTSC_WIDTH * Api::Video::Output::HEIGHT * sizeof(uint32_t), 0x80);
#else
   video_buffer = (uint32_t*)malloc(Api::Video::Output::NTSC_WIDTH * Api::Video::Output::HEIGHT * sizeof(uint32_t));
#endif

   machine = new Api::Machine(emulator);
   input = new Api::Input::Controllers;
   Api::User::fileIoCallback.Set(file_io_callback, 0);

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   check_system_specs();
}

void retro_deinit(void)
{
   if (machine->Is(Nes::Api::Machine::DISK))
   {
      if (fds)
         delete fds;
      fds = 0;
   }
   
   delete machine;
   delete video;
   delete audio;
   delete input;

   machine = 0;
   video   = 0;
   audio   = 0;
   input   = 0;

#ifdef _3DS
   linearFree(video_buffer);
#else
   free(video_buffer);
#endif
   video_buffer = NULL;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned, unsigned)
{
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Nestopia";
#ifdef GIT_VERSION
   info->library_version  = NST_VERSION GIT_VERSION;
#else
   info->library_version  = NST_VERSION;
#endif
   info->need_fullpath    = false;
   info->valid_extensions = "nes|fds|unf|unif";
}

double get_aspect_ratio(void)
{
  double aspect_ratio = is_pal ? NES_PAL_PAR : NES_NTSC_PAR;

  if (aspect_ratio_mode == 1)
    aspect_ratio = NES_NTSC_PAR;
  else if (aspect_ratio_mode == 2)
    aspect_ratio = NES_PAL_PAR;
  else if (aspect_ratio_mode == 3)
    aspect_ratio = NES_4_3_DAR;
    
  return aspect_ratio;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   const retro_system_timing timing = { is_pal ? 50.0 : 60.0, 44100.0 };
   info->timing = timing;

   // It's better if the size is based on NTSC_WIDTH if the filter is on
   const retro_game_geometry geom = {
      Api::Video::Output::WIDTH - (overscan_h ? 16 : 0),
      Api::Video::Output::HEIGHT - (overscan_v ? 16 : 0),
      Api::Video::Output::NTSC_WIDTH,
      Api::Video::Output::HEIGHT,
      get_aspect_ratio(),
   };
   info->geometry = geom;
}


void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   static const struct retro_variable vars[] = {
      { "nestopia_blargg_ntsc_filter", "Blargg NTSC filter; disabled|composite|svideo|rgb|monochrome" },
      { "nestopia_palette", "Palette; consumer|canonical|alternative|rgb|nescap|unsaturated-v7|pal|raw|custom" },
      { "nestopia_nospritelimit", "Remove 8-sprites-per-scanline hardware limit; disabled|enabled" },
      { "nestopia_fds_auto_insert", "Automatically insert first FDS disk on reset; enabled|disabled" },
      { "nestopia_overscan_v", "Mask Overscan (Vertical); enabled|disabled" },
      { "nestopia_overscan_h", "Mask Overscan (Horizontal); disabled|enabled" },
      { "nestopia_aspect" ,  "Preferred aspect ratio; auto|ntsc|pal|4:3" },
      { "nestopia_genie_distortion", "Game Genie Sound Distortion; disabled|enabled" },
      { "nestopia_favored_system", "Favored System; auto|ntsc|pal|famicom|dendy" },
      { "nestopia_ram_power_state", "RAM Power-on State; 0x00|0xFF|random" },
      { NULL, NULL },
   };

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_reset(void)
{
   machine->Reset(false);

   if (machine->Is(Nes::Api::Machine::DISK))
   {
      fds->EjectDisk();
      if (fds_auto_insert)
         fds->InsertDisk(0, 0);
   }
}

typedef struct
{
   unsigned retro;
   unsigned nes;
} keymap;

static const keymap bindmap[] = {
   { RETRO_DEVICE_ID_JOYPAD_A, Core::Input::Controllers::Pad::A },
   { RETRO_DEVICE_ID_JOYPAD_B, Core::Input::Controllers::Pad::B },
   { RETRO_DEVICE_ID_JOYPAD_SELECT, Core::Input::Controllers::Pad::SELECT },
   { RETRO_DEVICE_ID_JOYPAD_START, Core::Input::Controllers::Pad::START },
   { RETRO_DEVICE_ID_JOYPAD_UP, Core::Input::Controllers::Pad::UP },
   { RETRO_DEVICE_ID_JOYPAD_DOWN, Core::Input::Controllers::Pad::DOWN },
   { RETRO_DEVICE_ID_JOYPAD_LEFT, Core::Input::Controllers::Pad::LEFT },
   { RETRO_DEVICE_ID_JOYPAD_RIGHT, Core::Input::Controllers::Pad::RIGHT },
};

static void update_input()
{
   input_poll_cb();
   input->pad[0].buttons = 0;
   input->pad[1].buttons = 0;
   input->pad[2].buttons = 0;
   input->pad[3].buttons = 0;
   input->zapper.fire = 0;
   input->vsSystem.insertCoin = 0;
   
   if (Api::Input(emulator).GetConnectedController(1) == 5) {
      static int zapx = overscan_h ? 8 : 0; 
      static int zapy = overscan_v ? 8 : 0;
      zapx += input_state_cb(1, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_X);
      zapy += input_state_cb(1, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_Y);
   
      if (zapx >= 256) { crossx = 255; }
      else if (zapx <= 0) { crossx = 0; }
      else {crossx = zapx; }
      
      if (zapy >= 240) { crossy = 239; }
      else if (zapy <= 0) { crossy = 0; }
      else {crossy = zapy; }
      
      if (input_state_cb(1, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER)) {
         input->zapper.x = zapx;
         input->zapper.y = zapy;
         input->zapper.fire = 1;
      }
      
      if (input_state_cb(1, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TURBO)) {
         input->zapper.x = ~1U;
         input->zapper.fire = 1;
      }
   }
   
   for (unsigned p = 0; p < 4; p++)
      for (unsigned bind = 0; bind < sizeof(bindmap) / sizeof(bindmap[0]); bind++)
         input->pad[p].buttons |= input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, bindmap[bind].retro) ? bindmap[bind].nes : 0;
         
   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X))
      input->vsSystem.insertCoin |= Core::Input::Controllers::VsSystem::COIN_1;
      
   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))
      input->vsSystem.insertCoin |= Core::Input::Controllers::VsSystem::COIN_2;
      
   if (machine->Is(Nes::Api::Machine::DISK))
   {
      bool curL = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
      static bool prevL = false;
      if (curL && !prevL)
      {
         if (!fds->IsAnyDiskInserted())
            fds->InsertDisk(0, 0);
         else if (fds->CanChangeDiskSide())
            fds->ChangeSide();
      }
      prevL = curL;
      
      bool curR = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
      static bool prevR = false;
      if (curR && !prevR && (fds->GetNumDisks() > 1))
      {
         int currdisk = fds->GetCurrentDisk();
         fds->EjectDisk();
         fds->InsertDisk(!currdisk, 0);
      }
      prevR = curR;
   }
}

static void check_variables(void)
{
   static bool last_ntsc_val_same;
   struct retro_variable var = {0};
   struct retro_system_av_info av_info;

   Api::Sound sound(emulator);
   Api::Video video(emulator);
   Api::Video::RenderState renderState;
   Api::Machine machine(emulator);
   Api::Video::RenderState::Filter filter;

   var.key = "nestopia_favored_system";
   is_pal = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "auto") == 0)
      {
         if (dbpresent)
         {
            machine.SetMode(machine.GetDesiredMode());
            if (machine.GetMode() == Api::Machine::PAL)
            {
               is_pal = true;
               favsystem = Api::Machine::FAVORED_NES_PAL;
               machine.SetMode(Api::Machine::PAL);
            }
            else
            {
               favsystem = Api::Machine::FAVORED_NES_NTSC;
               machine.SetMode(Api::Machine::NTSC);
            }
         }
      }
      else if (strcmp(var.value, "ntsc") == 0)
      {
         favsystem = Api::Machine::FAVORED_NES_NTSC;
         machine.SetMode(Api::Machine::NTSC);
      }
      else if (strcmp(var.value, "pal") == 0)
      {
         favsystem = Api::Machine::FAVORED_NES_PAL;
         machine.SetMode(Api::Machine::PAL);
         is_pal = true;
      }
      else if (strcmp(var.value, "famicom") == 0)
      {
         favsystem = Api::Machine::FAVORED_FAMICOM;
         machine.SetMode(Api::Machine::NTSC);
      }
      else if (strcmp(var.value, "dendy") == 0)
      {
         favsystem = Api::Machine::FAVORED_DENDY;
         machine.SetMode(Api::Machine::PAL);
         is_pal = true;
      }
      else
      {
         favsystem = Api::Machine::FAVORED_NES_NTSC;
         machine.SetMode(Api::Machine::NTSC);
      }
   }
   if (audio) delete audio;
   audio = new Api::Sound::Output(audio_buffer, is_pal ? 44100 / 50 : 44100 / 60);

   var.key = "nestopia_genie_distortion";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "disabled") == 0)
         sound.SetGenie(0);
      else if (strcmp(var.value, "enabled") == 0)
         sound.SetGenie(1);
   }
   
   var.key = "nestopia_ram_power_state";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "0x00") == 0)
         machine.SetRamPowerState(0);
      else if (strcmp(var.value, "0xFF") == 0)
         machine.SetRamPowerState(1);
      else if (strcmp(var.value, "random") == 0)
         machine.SetRamPowerState(2);
   }

   var.key = "nestopia_nospritelimit";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "disabled") == 0)
         video.EnableUnlimSprites(false);
      else if (strcmp(var.value, "enabled") == 0)
         video.EnableUnlimSprites(true);
   }
   
   var.key = "nestopia_fds_auto_insert";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      fds_auto_insert = (strcmp(var.value, "enabled") == 0);
   
   var.key = "nestopia_blargg_ntsc_filter";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "disabled") == 0)
         blargg_ntsc = 0;
      else if (strcmp(var.value, "composite") == 0)
         blargg_ntsc = 2;
      else if (strcmp(var.value, "svideo") == 0)
         blargg_ntsc = 3;
      else if (strcmp(var.value, "rgb") == 0)
         blargg_ntsc = 4;
      else if (strcmp(var.value, "monochrome") == 0)
         blargg_ntsc = 5;
   }

   switch(blargg_ntsc)
   {
      case 0:
         filter = Api::Video::RenderState::FILTER_NONE;
         video_width = Api::Video::Output::WIDTH;
         video.SetSaturation(Api::Video::DEFAULT_SATURATION);
         break;
      case 2:
         filter = Api::Video::RenderState::FILTER_NTSC;
         video.SetSharpness(Api::Video::DEFAULT_SHARPNESS_COMP);
         video.SetColorResolution(Api::Video::DEFAULT_COLOR_RESOLUTION_COMP);
         video.SetColorBleed(Api::Video::DEFAULT_COLOR_BLEED_COMP);
         video.SetColorArtifacts(Api::Video::DEFAULT_COLOR_ARTIFACTS_COMP);
         video.SetColorFringing(Api::Video::DEFAULT_COLOR_FRINGING_COMP);
         video.SetSaturation(Api::Video::DEFAULT_SATURATION_COMP);
         video_width = Api::Video::Output::NTSC_WIDTH;
         break;
      case 3:
         filter = Api::Video::RenderState::FILTER_NTSC;
         video.SetSharpness(Api::Video::DEFAULT_SHARPNESS_SVIDEO);
         video.SetColorResolution(Api::Video::DEFAULT_COLOR_RESOLUTION_SVIDEO);
         video.SetColorBleed(Api::Video::DEFAULT_COLOR_BLEED_SVIDEO);
         video.SetColorArtifacts(Api::Video::DEFAULT_COLOR_ARTIFACTS_SVIDEO);
         video.SetColorFringing(Api::Video::DEFAULT_COLOR_FRINGING_SVIDEO);
         video.SetSaturation(Api::Video::DEFAULT_SATURATION_SVIDEO);
         video_width = Api::Video::Output::NTSC_WIDTH;
         break;
      case 4:
         filter = Api::Video::RenderState::FILTER_NTSC;
         video.SetSharpness(Api::Video::DEFAULT_SHARPNESS_RGB);
         video.SetColorResolution(Api::Video::DEFAULT_COLOR_RESOLUTION_RGB);
         video.SetColorBleed(Api::Video::DEFAULT_COLOR_BLEED_RGB);
         video.SetColorArtifacts(Api::Video::DEFAULT_COLOR_ARTIFACTS_RGB);
         video.SetColorFringing(Api::Video::DEFAULT_COLOR_FRINGING_RGB);
         video.SetSaturation(Api::Video::DEFAULT_SATURATION_RGB);
         video_width = Api::Video::Output::NTSC_WIDTH;
         break;
     case 5:
         filter = Api::Video::RenderState::FILTER_NTSC;
         video.SetSharpness(Api::Video::DEFAULT_SHARPNESS_MONO);
         video.SetColorResolution(Api::Video::DEFAULT_COLOR_RESOLUTION_MONO);
         video.SetColorBleed(Api::Video::DEFAULT_COLOR_BLEED_MONO);
         video.SetColorArtifacts(Api::Video::DEFAULT_COLOR_ARTIFACTS_MONO);
         video.SetColorFringing(Api::Video::DEFAULT_COLOR_FRINGING_MONO);
         video.SetSaturation(Api::Video::DEFAULT_SATURATION_MONO);
         video_width = Api::Video::Output::NTSC_WIDTH;
         break;
   }
   
   var.key = "nestopia_palette";
   
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "consumer") == 0) {
         video.GetPalette().SetMode(Api::Video::Palette::MODE_YUV);
         video.SetDecoder(Api::Video::DECODER_CONSUMER);
      }
      else if (strcmp(var.value, "canonical") == 0) {
         video.GetPalette().SetMode(Api::Video::Palette::MODE_YUV);
         video.SetDecoder(Api::Video::DECODER_CANONICAL);
      }
      else if (strcmp(var.value, "alternative") == 0) {
         video.GetPalette().SetMode(Api::Video::Palette::MODE_YUV);
         video.SetDecoder(Api::Video::DECODER_ALTERNATIVE);
      }
      else if (strcmp(var.value, "rgb") == 0) {
         video.GetPalette().SetMode(Api::Video::Palette::MODE_RGB);
      }
      else if (strcmp(var.value, "nescap") == 0) {
         video.GetPalette().SetMode(Api::Video::Palette::MODE_CUSTOM);
         video.GetPalette().SetCustom(nescap_palette, Api::Video::Palette::STD_PALETTE);
      }
      else if (strcmp(var.value, "unsaturated-v7") == 0) {
         video.GetPalette().SetMode(Api::Video::Palette::MODE_CUSTOM);
         video.GetPalette().SetCustom(unsaturated_v7_palette, Api::Video::Palette::STD_PALETTE);
      }
      else if (strcmp(var.value, "pal") == 0) {
         video.GetPalette().SetMode(Api::Video::Palette::MODE_CUSTOM);
         video.GetPalette().SetCustom(pal_palette, Api::Video::Palette::STD_PALETTE);
      }
      else if (strcmp(var.value, "raw") == 0) {
         /* outputs raw chroma/level/emphasis in the R/G/B channels
          * that can be decoded by the frontend (using shaders for example)
          * the following formulas can be used to extract the
          * values back from a normalized R/G/B triplet
          * chroma   = floor((R * 15.0) + 0.5)
          * level    = floor((G *  3.0) + 0.5)
          * emphasis = floor((B *  7.0) + 0.5) */
         unsigned char raw_palette[512][3];
         int i;
         for (i = 0; i < 512; i++)
         {
            raw_palette[i][0] = (((i >> 0) & 0xF) * 255) / 15;
            raw_palette[i][1] = (((i >> 4) & 0x3) * 255) / 3;
            raw_palette[i][2] = (((i >> 6) & 0x7) * 255) / 7;
         }
         video.GetPalette().SetMode(Api::Video::Palette::MODE_CUSTOM);
         video.GetPalette().SetCustom(raw_palette, Api::Video::Palette::EXT_PALETTE);
      }
      else if (strcmp(var.value, "custom") == 0) {
         video.GetPalette().SetMode(Api::Video::Palette::MODE_CUSTOM);
         video.GetPalette().SetCustom((const byte(*)[3])custpal, Api::Video::Palette::STD_PALETTE);
      }
   }
   
   var.key = "nestopia_overscan_v";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      overscan_v = (strcmp(var.value, "enabled") == 0);
   
   var.key = "nestopia_overscan_h";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      overscan_h = (strcmp(var.value, "enabled") == 0);

   var.key = "nestopia_aspect";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
     if (!strcmp(var.value, "ntsc"))
       aspect_ratio_mode = 1;
     else if (!strcmp(var.value, "pal"))
       aspect_ratio_mode = 2;
     else if (!strcmp(var.value, "4:3"))
       aspect_ratio_mode = 3;
     else
       aspect_ratio_mode = 0;
   }
   
   pitch = video_width * 4;
   
   renderState.filter = filter;
   renderState.width = video_width;
   renderState.height = Api::Video::Output::HEIGHT;
   renderState.bits.count = 32;
   renderState.bits.mask.r = 0x00ff0000;
   renderState.bits.mask.g = 0x0000ff00;
   renderState.bits.mask.b = 0x000000ff;
   if (NES_FAILED(video.SetRenderState( renderState )) && log_cb)
      log_cb(RETRO_LOG_INFO, "Nestopia core rejected render state\n");;

   retro_get_system_av_info(&av_info);
   environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);

}

void retro_run(void)
{
   update_input();
   emulator.Execute(video, audio, input);

   if (Api::Input(emulator).GetConnectedController(1) == 5)
      draw_crosshair(crossx, crossy);
   
   unsigned frames = is_pal ? 44100 / 50 : 44100 / 60;
   for (unsigned i = 0; i < frames; i++)
      audio_stereo_buffer[(i << 1) + 0] = audio_stereo_buffer[(i << 1) + 1] = audio_buffer[i];
   audio_batch_cb(audio_stereo_buffer, frames);
   
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
   {
      check_variables();
      delete video;
      video = 0;
      video = new Api::Video::Output(video_buffer, video_width * sizeof(uint32_t));
   }
   
   // Absolute mess of inline if statements...
   video_cb(video_buffer + (overscan_v ? ((overscan_h ? 8 : 0) + (blargg_ntsc ? Api::Video::Output::NTSC_WIDTH : Api::Video::Output::WIDTH) * 8) : (overscan_h ? 8 : 0) + 0),
         video_width - (overscan_h ? 16 : 0),
         Api::Video::Output::HEIGHT - (overscan_v ? 16 : 0),
         pitch);
}

static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
      buf[0] = '\0';
}


bool retro_load_game(const struct retro_game_info *info)
{
   const char *dir;
   char slash;
   char db_path[256];
   char palette_path[256];
   
#if defined(_WIN32)
   slash = '\\';
#else
   slash = '/';
#endif

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "(VSSystem) Coin 1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "(VSSystem) Coin 2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "(FDS) Disk Side Change" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "(FDS) Eject Disk" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "(VSSystem) Coin 1" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "(VSSystem) Coin 2" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "(FDS) Disk Side Change" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "(FDS) Eject Disk" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Select" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "(VSSystem) Coin 1" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "(VSSystem) Coin 2" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "(FDS) Disk Side Change" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "(FDS) Eject Disk" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Select" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "(VSSystem) Coin 1" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "(VSSystem) Coin 2" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "(FDS) Disk Side Change" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "(FDS) Eject Disk" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Select" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },

      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) || !dir)
      return false;

   snprintf(palette_path, sizeof(palette_path), "%s%ccustom.pal", dir, slash);

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "Custom palette path: %s\n", palette_path);
   
   std::ifstream *custompalette = new std::ifstream(palette_path, std::ifstream::in|std::ifstream::binary);
   
   if (custompalette->is_open())
   {
      custompalette->read((char*)custpal, 64*3);
      if (log_cb)
         log_cb(RETRO_LOG_WARN, "custom.pal loaded from system directory.\n");
   }
   else
   {
      if (log_cb)
         log_cb(RETRO_LOG_WARN, "custom.pal not found in system directory.\n");
   }
   delete custompalette;
   
   snprintf(db_path, sizeof(db_path), "%s%cNstDatabase.xml", dir, slash);

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "NstDatabase.xml path: %s\n", db_path);
   
   Api::Cartridge::Database database(emulator);
   std::ifstream *db_file = new std::ifstream(db_path, std::ifstream::in|std::ifstream::binary);
   
   if (db_file->is_open())
   {
      database.Load(*db_file);
      database.Enable(true);
      dbpresent = true;
   }
   else
   {
      if (log_cb)
         log_cb(RETRO_LOG_WARN, "NstDatabase.xml required to detect region and some mappers.\n");
      delete db_file;
      dbpresent = false;
   }
   
   if (info->path != NULL)
   {
      extract_basename(g_basename, info->path, sizeof(g_basename));
      extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));
   }
   
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "XRGB8888 is not supported.\n");
      return false;
   }
   
   std::stringstream ss(std::string(reinterpret_cast<const char*>(info->data),
            reinterpret_cast<const char*>(info->data) + info->size));

   if (info->path && (strstr(info->path, ".fds") || strstr(info->path, ".FDS")))
   {
      fds = new Api::Fds(emulator);

      if (fds)
      {
         char fds_bios_path[256];
         /* search for BIOS in system directory */
         bool found = false;

         snprintf(fds_bios_path, sizeof(fds_bios_path), "%s%cdisksys.rom", dir, slash);
         if (log_cb)
            log_cb(RETRO_LOG_INFO, "FDS BIOS path: %s\n", fds_bios_path);

         std::ifstream *fds_bios_file = new std::ifstream(fds_bios_path, std::ifstream::in|std::ifstream::binary);

         if (fds_bios_file->is_open())
            fds->SetBIOS(fds_bios_file);
         else
         {
            delete fds_bios_file;
            return false;
         }
      }
      else
         return false;
   }
   
   if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &g_save_dir))
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Could not find save directory.\n");
   }

   is_pal = false;
   check_variables();

   if (machine->Load(ss, favsystem))
      return false;

   Api::Video ivideo(emulator);
   ivideo.SetSharpness(Api::Video::DEFAULT_SHARPNESS_RGB);
   ivideo.SetColorResolution(Api::Video::DEFAULT_COLOR_RESOLUTION_RGB);
   ivideo.SetColorBleed(Api::Video::DEFAULT_COLOR_BLEED_RGB);
   ivideo.SetColorArtifacts(Api::Video::DEFAULT_COLOR_ARTIFACTS_RGB);
   ivideo.SetColorFringing(Api::Video::DEFAULT_COLOR_FRINGING_RGB);

   Api::Video::RenderState state;
   state.filter = Api::Video::RenderState::FILTER_NONE;
   state.width = 256;
   state.height = 240;
   state.bits.count = 32;
   state.bits.mask.r = 0x00ff0000;
   state.bits.mask.g = 0x0000ff00;
   state.bits.mask.b = 0x000000ff;
   ivideo.SetRenderState(state);

   Api::Sound isound(emulator);
   isound.SetSampleBits(16);
   isound.SetSampleRate(44100);
   isound.SetSpeaker(Api::Sound::SPEAKER_MONO);

   if (dbpresent)
   {
      Api::Input(emulator).AutoSelectController(0);
      Api::Input(emulator).AutoSelectController(1);
      Api::Input(emulator).AutoSelectController(2);
      Api::Input(emulator).AutoSelectController(3);
   }
   else
   {
      Api::Input(emulator).ConnectController(0, Api::Input::PAD1);
      Api::Input(emulator).ConnectController(1, Api::Input::PAD2);
      //Api::Input(emulator).ConnectController(1, Api::Input::ZAPPER);
      Api::Input(emulator).ConnectController(2, Api::Input::PAD3);
      Api::Input(emulator).ConnectController(3, Api::Input::PAD4);
   }

   machine->Power(true);

   check_variables();

   if (fds_auto_insert && machine->Is(Nes::Api::Machine::DISK))
      fds->InsertDisk(0, 0);
   
   video = new Api::Video::Output(video_buffer, video_width * sizeof(uint32_t));
   
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "[Nestopia]: Machine is %s.\n", is_pal ? "PAL" : "NTSC");

   return true;
}

void retro_unload_game(void)
{
   machine->Unload();
   sram = 0;
   sram_size = 0;
}

unsigned retro_get_region(void)
{
   return is_pal ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t)
{
   return false;
}

size_t retro_serialize_size(void)
{
   std::stringstream ss;
   if (machine->SaveState(ss, Api::Machine::NO_COMPRESSION))
      return 0;
   return ss.str().size();
}

bool retro_serialize(void *data, size_t size)
{
   std::stringstream ss;
   if (machine->SaveState(ss, Api::Machine::NO_COMPRESSION))
      return false;

   std::string state = ss.str();
   if (state.size() > size)
      return false;

   std::copy(state.begin(), state.end(), reinterpret_cast<char*>(data));
   return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   std::stringstream ss(std::string(reinterpret_cast<const char*>(data),
            reinterpret_cast<const char*>(data) + size));
   return !machine->LoadState(ss);
}

void *retro_get_memory_data(unsigned id)
{
   if (id != RETRO_MEMORY_SAVE_RAM)
      return 0;

   return sram;
}

size_t retro_get_memory_size(unsigned id)
{
   if (id != RETRO_MEMORY_SAVE_RAM)
      return 0;

   return sram_size;
}

void retro_cheat_reset(void)
{
   Nes::Api::Cheats cheater(emulator);
   cheater.ClearCodes();
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   Nes::Api::Cheats cheater(emulator);
   Nes::Api::Cheats::Code ggCode;

   if (Nes::Api::Cheats::GameGenieDecode(code, ggCode) == RESULT_OK)
      cheater.SetCode(ggCode);
   if (Nes::Api::Cheats::ProActionRockyDecode(code, ggCode) == RESULT_OK)
      cheater.SetCode(ggCode);
}
