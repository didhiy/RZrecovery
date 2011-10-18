/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */  
  
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
  
#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"
#include "encryptedfs_provisioning.h"
  
#include "flashutils/flashutils.h"
#include "mounts.h"
static const struct option OPTIONS[] = { 
    {"send_intent", required_argument, NULL, 's'}, 
  {"update_package", required_argument, NULL, 'u'}, 
  {"wipe_data", no_argument, NULL, 'w'}, 
  {"wipe_cache", no_argument, NULL, 'c'}, 
  {"set_encrypted_filesystems", required_argument, NULL, 'e'}, 
  {NULL, 0, NULL, 0}, 
};

 static const char *COMMAND_FILE = "/cache/recovery/command";
static const char *INTENT_FILE = "/cache/recovery/intent";
static const char *LOG_FILE = "/cache/recovery/log";
static const char *LAST_LOG_FILE = "/cache/recovery/last_log";
static const char *SDCARD_ROOT = "/sdcard";
static const char *TEMPORARY_LOG_FILE = "/tmp/recovery.log";
static const char *SIDELOAD_TEMP_DIR = "/tmp/sideload";

 
/*
 * The recovery tool communicates with the main system through /cache files.
 *   /cache/recovery/command - INPUT - command line for tool, one arg per line
 *   /cache/recovery/log - OUTPUT - combined log file from recovery run(s)
 *   /cache/recovery/intent - OUTPUT - intent that was passed in
 *
 * The arguments which may be supplied in the recovery.command file:
 *   --send_intent=anystring - write the text out to recovery.intent
 *   --update_package=path - verify install an OTA package file
 *   --wipe_data - erase user data (and cache), then reboot
 *   --wipe_cache - wipe cache (but not user data), then reboot
 *   --set_encrypted_filesystem=on|off - enables / diasables encrypted fs
 *
 * After completing, we remove /cache/recovery/command and reboot.
 * Arguments may also be supplied in the bootloader control block (BCB).
 * These important scenarios must be safely restartable at any point:
 *
 * FACTORY RESET
 * 1. user selects "factory reset"
 * 2. main system writes "--wipe_data" to /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--wipe_data"
 *    -- after this, rebooting will restart the erase --
 * 5. erase_volume() reformats /data
 * 6. erase_volume() reformats /cache
 * 7. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 8. main() calls reboot() to boot main system
 *
 * OTA INSTALL
 * 1. main system downloads OTA package to /cache/some-filename.zip
 * 2. main system writes "--update_package=/cache/some-filename.zip"
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--update_package=..."
 *    -- after this, rebooting will attempt to reinstall the update --
 * 5. install_package() attempts to install the update
 *    NOTE: the package install must itself be restartable from any point
 * 6. finish_recovery() erases BCB
 *    -- after this, rebooting will (try to) restart the main system --
 * 7. ** if install failed **
 *    7a. prompt_and_wait() shows an error icon and waits for the user
 *    7b; the user reboots (pulling the battery, etc) into the main system
 * 8. main() calls maybe_install_firmware_update()
 *    ** if the update contained radio/hboot firmware **:
 *    8a. m_i_f_u() writes BCB with "boot-recovery" and "--wipe_cache"
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8b. m_i_f_u() writes firmware image into raw cache partition
 *    8c. m_i_f_u() writes BCB with "update-radio/hboot" and "--wipe_cache"
 *        -- after this, rebooting will attempt to reinstall firmware --
 *    8d. bootloader tries to flash firmware
 *    8e. bootloader writes BCB with "boot-recovery" (keeping "--wipe_cache")
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8f. erase_volume() reformats /cache
 *    8g. finish_recovery() erases BCB
 *        -- after this, rebooting will (try to) restart the main system --
 * 9. main() calls reboot() to boot main system
 *
 * SECURE FILE SYSTEMS ENABLE/DISABLE
 * 1. user selects "enable encrypted file systems"
 * 2. main system writes "--set_encrypted_filesystems=on|off" to
 *    /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and
 *    "--set_encrypted_filesystems=on|off"
 *    -- after this, rebooting will restart the transition --
 * 5. read_encrypted_fs_info() retrieves encrypted file systems settings from /data
 *    Settings include: property to specify the Encrypted FS istatus and
 *    FS encryption key if enabled (not yet implemented)
 * 6. erase_volume() reformats /data
 * 7. erase_volume() reformats /cache
 * 8. restore_encrypted_fs_info() writes required encrypted file systems settings to /data
 *    Settings include: property to specify the Encrypted FS status and
 *    FS encryption key if enabled (not yet implemented)
 * 9. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 10. main() calls reboot() to boot main system
 */ 
static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;

 
// open a given path, mounting partitions as necessary
  FILE * fopen_path (const char *path, const char *mode)
{
  if (ensure_path_mounted (path) != 0)
	  {
	    LOGE ("Can't mount %s\n", path);
	    return NULL;
	  }
   
    // When writing, try to create the containing directory, if necessary.
    // Use generous permissions, the system (init.rc) will reset them.
    if (strchr ("wa", mode[0]))
    dirCreateHierarchy (path, 0777, NULL, 1);
   FILE * fp = fopen (path, mode);
  return fp;
}

 
// close a file, log an error if the error indicator is set
void 
check_and_fclose (FILE * fp, const char *name)
{
  fflush (fp);
  if (ferror (fp))
    LOGE ("Error in %s\n(%s)\n", name, strerror (errno));
  fclose (fp);
}

  void
set_cpufreq (char *speed)
{
  FILE * fs =
    fopen ("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "w");
  fputs (speed, fs);
  fputs ("\n", fs);
  printf ("\nMax cpu slot set to ");
  printf ("%s", speed);
  printf ("\n");
  fclose (fs);
}  void

write_fstab_root (char *path, FILE * file) 
{
  Volume * vol = volume_for_path (path);
  if (vol == NULL)
	  {
	    LOGW
	      ("Unable to get recovery.fstab info for %s during fstab generation!\n",
	       path);
	    return;
	  }
   char device[200];

  if (vol->device[0] != '/')
    get_partition_device (vol->device, device);
  
  else
    strcpy (device, vol->device);
   fprintf (file, "%s ", device);
  fprintf (file, "%s ", path);
  
    // special case rfs cause auto will mount it as vfat on samsung.
    fprintf (file, "%s rw\n", vol->fs_type2 != NULL
	     && strcmp (vol->fs_type, "rfs") != 0 ? "auto" : vol->fs_type);
}

 void
process_volumes ()
{
  create_fstab ();
  printf ("process_volumes done.");
}  void

create_fstab () 
{
  struct stat info;

  system ("touch /etc/mtab");
  FILE * file = fopen ("/etc/fstab", "w");
  if (file == NULL)
	  {
	    LOGW ("Unable to create /etc/fstab!\n");
	    return;
	  }
  Volume * vol = volume_for_path ("/boot");
  if (NULL != vol && strcmp (vol->fs_type, "mtd") != 0
       && strcmp (vol->fs_type, "emmc") != 0)
    write_fstab_root ("/boot", file);
  write_fstab_root ("/cache", file);
  write_fstab_root ("/data", file);
  write_fstab_root ("/system", file);
  write_fstab_root ("/sdcard", file);
  write_fstab_root ("/sd-ext", file);
  fclose (file);
  LOGI ("Completed outputting fstab.\n\n");
}

 
//write recovery files from cache to sdcard
  void
write_files ()
{
  if (ensure_path_mounted ("/sdcard") != 0)
	  {
	    LOGE ("Can't mount /sdcard\n");
	    return;
	  }
  else
	  {
	    if (access ("/cache/rgb", F_OK) != -1)
		    {
		      system ("cp /cache/rgb /sdcard/RZR/rgb");
		      printf ("\nColors file saved to sdcard.\n");
		    }
	    if (access ("/cache/oc", F_OK) != -1)
		    {
		      system ("cp /cache/oc /sdcard/RZR/oc");
		      printf ("\nOverclock file saved to sdcard.\n");
		    }
	    if (access ("/cache/rnd", F_OK) != -1)
		    {
		      system ("cp /cache/rnd /sdcard/RZR/rnd");
		      printf ("\nRave file saved to sdcard.\n");
		    }
	    sync ();
	  }
}

 void
read_cpufreq ()
{
  ensure_path_mounted ("/sdcard/RZR");
  if (access ("/sdcard/RZR/oc", F_OK) != -1)
	  {
	    system ("cp /sdcard/RZR/oc /cache/oc");
	    printf ("\nCopied /sdcard/RZR/oc to /cache/oc.\n");
	  }
  else
	  {
	    mkdir ("/sdcard/RZR", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	  }
   if (access ("/cache/oc", F_OK) != -1)
	  {
	    FILE * fs = fopen ("/cache/oc", "r");
	    char *freq = calloc (9, sizeof (char));

	    fgets (freq, 9, fs);
	    fclose (fs);
	    if (access
		 ("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
		  F_OK) != -1)
		    {
		      set_cpufreq (freq);
		    }
	  }
  sync ();
}

 
//read recovery files from sdcard to cache
  void
read_files ()
{
  ensure_path_mounted ("/sdcard/RZR");
  if (access ("/sdcard/RZR/rgb", F_OK) != -1)
	  {
	    system ("cp /sdcard/RZR/rgb /cache/rgb");
	    printf ("\nCopied /sdcard/RZR/rgb to /cache/rgb.\n");
	  }
  else
	  {
	    mkdir ("/sdcard/RZR", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	    set_color (54, 74, 255);
	  }
   if (access ("/sdcard/RZR/rnd", F_OK) != -1)
	  {
	    system ("cp /sdcard/RZR/rnd /cache/rnd");
	    printf ("\nCopied /sdcard/RZR/rnd to /cache/rnd.\n");
	  }
  else
	  {
	    mkdir ("/sdcard/RZR", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	  }
   sync ();
  ensure_path_unmounted ("/sdcard/RZR");
}

void activateLEDs() 
{
// turn on button backlights if available
  if (!access ("/sys/class/leds/button-backlight/brightness", F_OK)) {
	FILE *cbb = fopen ("/sys/class/leds/button-backlight/brightness", "w");
	fputs("255",cbb);
	fputs("\n",cbb);
	fclose(cbb);
  }
  if (!access ("/sys/class/leds/button-backlight-portait/brightness", F_OK)) {
          FILE *cbp = fopen ("/sys/class/leds/button-backlight-portait/brightness", "w");
	  fputs("255",cbp);
	  fputs("\n",cbp);
	  fclose(cbp);
  }
}

 
// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
void 
get_args (int *argc, char ***argv)
{
  struct bootloader_message boot;

  memset (&boot, 0, sizeof (boot));
  get_bootloader_message (&boot);	// this may fail, leaving a zeroed structure
  if (boot.command[0] != 0 && boot.command[0] != 255)
	  {
	    LOGI ("Boot command: %.*s\n", sizeof (boot.command),
		   boot.command);
	  }
   if (boot.status[0] != 0 && boot.status[0] != 255)
	  {
	    LOGI ("Boot status: %.*s\n", sizeof (boot.status), boot.status);
	  }
   
    // --- if arguments weren't supplied, look in the bootloader control block
    if (*argc <= 1)
	  {
	    boot.recovery[sizeof (boot.recovery) - 1] = '\0';	// Ensure termination
	    const char *arg = strtok (boot.recovery, "\n");

	    if (arg != NULL && !strcmp (arg, "recovery"))
		    {
		      *argv = (char **) malloc (sizeof (char *) * MAX_ARGS);
		      (*argv)[0] = strdup (arg);
		      for (*argc = 1; *argc < MAX_ARGS; ++*argc)
			      {
				if ((arg = strtok (NULL, "\n")) == NULL)
				  break;
				(*argv)[*argc] = strdup (arg);
			      }
		      LOGI ("Got arguments from boot message\n");
		    }
	    else if (boot.recovery[0] != 0 && boot.recovery[0] != 255)
		    {
		      LOGE ("Bad boot message\n\"%.20s\"\n", boot.recovery);
		    }
	  }
   
    // --- if that doesn't work, try the command file
    if (*argc <= 1)
	  {
	    FILE * fp = fopen_path (COMMAND_FILE, "r");
	    if (fp != NULL)
		    {
		      char *argv0 = (*argv)[0];

		      *argv = (char **) malloc (sizeof (char *) * MAX_ARGS);
		      (*argv)[0] = argv0;	// use the same program name
		      char buf[MAX_ARG_LENGTH];

		      for (*argc = 1; *argc < MAX_ARGS; ++*argc)
			      {
				if (!fgets (buf, sizeof (buf), fp))
				  break;
				(*argv)[*argc] = strdup (strtok (buf, "\r\n"));	// Strip newline.
			      }
		       check_and_fclose (fp, COMMAND_FILE);
		      LOGI ("Got arguments from %s\n", COMMAND_FILE);
		    }
	  }
   
    // --> write the arguments we have back into the bootloader control block
    // always boot into recovery after this (until finish_recovery() is called)
    strlcpy (boot.command, "boot-recovery", sizeof (boot.command));
  strlcpy (boot.recovery, "recovery\n", sizeof (boot.recovery));
  int i;

  for (i = 1; i < *argc; ++i)
	  {
	    strlcat (boot.recovery, (*argv)[i], sizeof (boot.recovery));
	    strlcat (boot.recovery, "\n", sizeof (boot.recovery));
	  }
  set_bootloader_message (&boot);
}

 void 
set_sdcard_update_bootloader_message ()
{
  struct bootloader_message boot;

  memset (&boot, 0, sizeof (boot));
  strlcpy (boot.command, "boot-recovery", sizeof (boot.command));
  strlcpy (boot.recovery, "recovery\n", sizeof (boot.recovery));
  set_bootloader_message (&boot);
}

 
// How much of the temp log we have copied to the copy in cache.
long tmplog_offset = 0;
 void 
copy_log_file (const char *destination, int append)
{
  FILE * log = fopen_path (destination, append ? "a" : "w");
  if (log == NULL)
	  {
	    LOGE ("Can't open %s\n", destination);
	  }
  else
	  {
	    FILE * tmplog = fopen (TEMPORARY_LOG_FILE, "r");
	    if (tmplog == NULL)
		    {
		      LOGE ("Can't open %s\n", TEMPORARY_LOG_FILE);
		    }
	    else
		    {
		      if (append)
			      {
				fseek (tmplog, tmplog_offset, SEEK_SET);	// Since last write
			      }
		      char buf[4096];

		      while (fgets (buf, sizeof (buf), tmplog))
			fputs (buf, log);
		      if (append)
			      {
				tmplog_offset = ftell (tmplog);
			      }
		      check_and_fclose (tmplog, TEMPORARY_LOG_FILE);
		    }
	    check_and_fclose (log, destination);
	  }
}

  
// clear the recovery command and prepare to boot a (hopefully working) system,
// copy our log file to cache as well (for the system to read), and
// record any intent we were asked to communicate back to the system.
// this function is idempotent: call it as many times as you like.
void 
finish_recovery (const char *send_intent)
{
  
    // By this point, we're ready to return to the main system...
    if (send_intent != NULL)
	  {
	    FILE * fp = fopen_path (INTENT_FILE, "w");
	    if (fp == NULL)
		    {
		      LOGE ("Can't open %s\n", INTENT_FILE);
		    }
	    else
		    {
		      fputs (send_intent, fp);
		      check_and_fclose (fp, INTENT_FILE);
		    }
	  }
   
    // Copy logs to cache so the system can find out what happened.
    copy_log_file (LOG_FILE, true);
  copy_log_file (LAST_LOG_FILE, false);
  chmod (LAST_LOG_FILE, 0640);
   
    // Reset to mormal system boot so recovery won't cycle indefinitely.
  struct bootloader_message boot;

  memset (&boot, 0, sizeof (boot));
  set_bootloader_message (&boot);
   
    // Remove the command file, so recovery won't repeat indefinitely.
    if (ensure_path_mounted (COMMAND_FILE) != 0 || 
	(unlink (COMMAND_FILE) && errno != ENOENT))
	  {
	    LOGW ("Can't unlink %s\n", COMMAND_FILE);
	  }
   sync ();			// For good measure.
}

 int 
erase_volume (const char *volume)
{
  ui_set_background (BACKGROUND_ICON_RZ);
  ui_show_indeterminate_progress ();
  ui_print ("Formatting %s...\n", volume);
   if (strcmp (volume, "/cache") == 0)
	  {
	    
	      // Any part of the log we'd copied to cache is now gone.
	      // Reset the pointer so we copy from the beginning of the temp
	      // log.
	      tmplog_offset = 0;
	  }
   return format_volume (volume);
}

 int 
erase_volume_cmd (int argc, char **argv)
{
  if (argc != 2)
	  {
	    printf ("\nUsage: %s volume\n", argv[0]);
	    return 0;
	  }
  char **volume = argv[1];

  if (strcmp (volume, "/cache") == 0)
	  {
	    
	      // Any part of the log we'd copied to cache is now gone.
	      // Reset the pointer so we copy from the beginning of the temp
	      // log.
	      tmplog_offset = 0;
	  }
  return format_volume (volume);
}

 char *
copy_sideloaded_package (const char *original_path)
{
  if (ensure_path_mounted (original_path) != 0)
	  {
	    LOGE ("Can't mount %s\n", original_path);
	    return NULL;
	  }
   if (ensure_path_mounted (SIDELOAD_TEMP_DIR) != 0)
	  {
	    LOGE ("Can't mount %s\n", SIDELOAD_TEMP_DIR);
	    return NULL;
	  }
   if (mkdir (SIDELOAD_TEMP_DIR, 0700) != 0)
	  {
	    if (errno != EEXIST)
		    {
		      LOGE ("Can't mkdir %s (%s)\n", SIDELOAD_TEMP_DIR,
			     strerror (errno));
		      return NULL;
		    }
	  }
   
    // verify that SIDELOAD_TEMP_DIR is exactly what we expect: a
    // directory, owned by root, readable and writable only by root.
  struct stat st;

  if (stat (SIDELOAD_TEMP_DIR, &st) != 0)
	  {
	    LOGE ("failed to stat %s (%s)\n", SIDELOAD_TEMP_DIR,
		   strerror (errno));
	    return NULL;
	  }
  if (!S_ISDIR (st.st_mode))
	  {
	    LOGE ("%s isn't a directory\n", SIDELOAD_TEMP_DIR);
	    return NULL;
	  }
  if ((st.st_mode & 0777) != 0700)
	  {
	    LOGE ("%s has perms %o\n", SIDELOAD_TEMP_DIR, st.st_mode);
	    return NULL;
	  }
  if (st.st_uid != 0)
	  {
	    LOGE ("%s owned by %lu; not root\n", SIDELOAD_TEMP_DIR,
		   st.st_uid);
	    return NULL;
	  }
   char copy_path[PATH_MAX];

  strcpy (copy_path, SIDELOAD_TEMP_DIR);
  strcat (copy_path, "/package.zip");
   char *buffer = malloc (BUFSIZ);

  if (buffer == NULL)
	  {
	    LOGE ("Failed to allocate buffer\n");
	    return NULL;
	  }
   size_t read;
  FILE * fin = fopen (original_path, "rb");
  if (fin == NULL)
	  {
	    LOGE ("Failed to open %s (%s)\n", original_path,
		   strerror (errno));
	    return NULL;
	  }
  FILE * fout = fopen (copy_path, "wb");
  if (fout == NULL)
	  {
	    LOGE ("Failed to open %s (%s)\n", copy_path, strerror (errno));
	    return NULL;
	  }
   while ((read = fread (buffer, 1, BUFSIZ, fin)) > 0)
	  {
	    if (fwrite (buffer, 1, read, fout) != read)
		    {
		      LOGE ("Short write of %s (%s)\n", copy_path,
			     strerror (errno));
		      return NULL;
		    }
	  }
   free (buffer);
   if (fclose (fout) != 0)
	  {
	    LOGE ("Failed to close %s (%s)\n", copy_path, strerror (errno));
	    return NULL;
	  }
   if (fclose (fin) != 0)
	  {
	    LOGE ("Failed to close %s (%s)\n", original_path,
		   strerror (errno));
	    return NULL;
	  }
   
    // "adb push" is happy to overwrite read-only files when it's
    // running as root, but we'll try anyway.
    if (chmod (copy_path, 0400) != 0)
	  {
	    LOGE ("Failed to chmod %s (%s)\n", copy_path, strerror (errno));
	    return NULL;
	  }
   return strdup (copy_path);
}

 char **
prepend_title (const char **headers)
{
   FILE * f = fopen ("/recovery.version", "r");
  char *vers = calloc (20, sizeof (char));

  fgets (vers, 20, f);
   strtok (vers, " ");	// truncate vers to before the first space
  char *patch_line_ptr = calloc ((strlen (headers[0]) + 21), sizeof (char));
  char *patch_line = patch_line_ptr;

  strcpy (patch_line, headers[0]);
  strcat (patch_line, " (");
  strcat (patch_line, vers);
  strcat (patch_line, ")");
   int c = 0;
  char **p1;

  for (p1 = headers; *p1; ++p1, ++c);
   char **ver_headers = calloc ((c + 1), sizeof (char *));

  ver_headers[0] = patch_line;
  ver_headers[c] = NULL;
  char **v = ver_headers + 1;

  for (p1 = headers + 1; *p1; ++p1, ++v)
    *v = *p1;
   
    // count the number of lines in our title, plus the
    // caller-provided headers.
  int count = 0;
  char **p;

  for (p = headers; *p; ++p, ++count);
   char **new_headers = malloc ((count + 1) * sizeof (char *));
  char **h = new_headers;

  for (p = ver_headers; *p; ++p, ++h)
    *h = *p;
  *h = NULL;
   return new_headers;
}

 int 
get_menu_selection (char **headers, char **items, int menu_only,
		    int initial_selection)
{
  
    // throw away keys pressed previously, so user doesn't
    // accidentally trigger menu items.
    ui_clear_key_queue ();
   ui_start_menu (headers, items, initial_selection);
  int selected = initial_selection;
  int chosen_item = -1;

   while (chosen_item < 0)
	  {
	    int key = ui_wait_key ();

	     int action = device_handle_key (key);

	     if (action < 0)
		    {
		      switch (action)
			      {
			      case HIGHLIGHT_UP:
				--selected;
				selected = ui_menu_select (selected);
				break;
			      case HIGHLIGHT_DOWN:
				++selected;
				selected = ui_menu_select (selected);
				break;
			      case SELECT_ITEM:
				chosen_item = selected;
				break;
			      case NO_ACTION:
				break;
			      case ITEM_BACK:
				return (ITEM_BACK);
				break;
			      }
		    }
	    else if (!menu_only)
		    {
		      chosen_item = action;
		    }
	  }
   ui_end_menu ();
  return chosen_item;
}

 int
compare_string (const void *a, const void *b)
{
  return strcmp (*(const char **) a, *(const char **) b);
}  void 

prompt_and_wait ()
{
  char **headers = prepend_title ((const char **) MENU_HEADERS);

   for (;;)
	  {
	    finish_recovery (NULL);
	    ui_reset_progress ();
	     int chosen_item =
	      get_menu_selection (headers, MENU_ITEMS, 1,
				  chosen_item < 0 ? 0 : chosen_item);
	    
	      // device-specific code may take some action here.  It may
	      // return one of the core actions handled in the switch
	      // statement below.
	      chosen_item = device_perform_action (chosen_item);
	     switch (chosen_item)
		    {
		    case MAIN_REBOOT:
		      reboot_android ();
		      break;
		     case MAIN_RECOVERY:
		      reboot_recovery ();
		      break;
		     case MAIN_SHUTDOWN:
		      power_off ();
		      break;
		     case MAIN_BOOTLOADER:
		      reboot_bootloader();
		      break;
		     case MAIN_WIPE_MENU:
		      show_wipe_menu ();
		      break;
		     case MAIN_MOUNTS:
		      show_mount_menu ();
		      break;
		     case MAIN_NANDROID:
		      show_nandroid_menu ();
		      break;
		     case MAIN_INSTALL:
		      choose_file_menu ("/sdcard/");
		      break;
		     case MAIN_EXTRAS:
		      show_extras_menu ();
		      break;
		    }
	  }
}

 void 
print_property (const char *key, const char *name, void *cookie)
{
  printf ("%s=%s\n", key, name);
}  int 

main (int argc, char **argv)
{
  if (strstr (argv[0], "recovery") == NULL)
	 {
	    if (strstr (argv[0], "flash_image") != NULL)
	      return flash_image_main (argc, argv);
	    if (strstr (argv[0], "dump_image") != NULL)
	      return dump_image_main (argc, argv);
	    if (strstr (argv[0], "erase_image") != NULL)
	      return erase_image_main (argc, argv);
	    if (strstr (argv[0], "format") != NULL)
	      return erase_volume_cmd (argc, argv);
	    //we dont need to keep executing stuff past this point if an embedded function was called 
	    return 0;
	  }
  time_t start = time (NULL);
   
    // If these fail, there's not really anywhere to complain...
    freopen (TEMPORARY_LOG_FILE, "a", stdout);
  setbuf (stdout, NULL);
  freopen (TEMPORARY_LOG_FILE, "a", stderr);
  setbuf (stderr, NULL);
  printf ("Starting recovery on %s", ctime (&start));
  load_volume_table ();
  process_volumes ();
  read_files ();
  ui_init ();
  activateLEDs();
  ui_set_background (BACKGROUND_ICON_RZ);
  get_args (&argc, &argv);
   int previous_runs = 0;
  const char *send_intent = NULL;
  const char *update_package = NULL;
  const char *encrypted_fs_mode = NULL;
  int wipe_data = 0, wipe_cache = 0;
  int toggle_secure_fs = 0;

  encrypted_fs_info encrypted_fs_data;
    int arg;

  while ((arg = getopt_long (argc, argv, "", OPTIONS, NULL)) != -1)
	  {
	    switch (arg)
		    {
		    case 'p':
		      previous_runs = atoi (optarg);
		      break;
		    case 's':
		      send_intent = optarg;
		      break;
		    case 'u':
		      update_package = optarg;
		      break;
		    case 'w':
		      wipe_data = wipe_cache = 1;
		      break;
		    case 'c':
		      wipe_cache = 1;
		      break;
		    case 'e':
		      encrypted_fs_mode = optarg;
		      toggle_secure_fs = 1;
		      break;
		    case '?':
		      LOGE ("Invalid command argument\n");
		      continue;
		    }
	  }
   device_recovery_start ();
  read_cpufreq ();
   printf ("Command:");
  for (arg = 0; arg < argc; arg++)
	  {
	    printf (" \"%s\"", argv[arg]);
	  }
  printf ("\n");
   if (update_package)
	  {
	    
	      // For backwards compatibility on the cache partition only, if
	      // we're given an old 'root' path "CACHE:foo", change it to
	      // "/cache/foo".
	      if (strncmp (update_package, "CACHE:", 6) == 0)
		    {
		      int len = strlen (update_package) + 10;
		      char *modified_path = malloc (len);

		      strlcpy (modified_path, "/cache/", len);
		      strlcat (modified_path, update_package + 6, len);
		      printf ("(replacing path \"%s\" with \"%s\")\n",
			       update_package, modified_path);
		      update_package = modified_path;
		    }
	  }
  printf ("\n");
   property_list (print_property, NULL);
  printf ("\n");
   int status = INSTALL_SUCCESS;

   if (toggle_secure_fs)
	  {
	    if (strcmp (encrypted_fs_mode, "on") == 0)
		    {
		      encrypted_fs_data.mode = MODE_ENCRYPTED_FS_ENABLED;
		      ui_print ("Enabling Encrypted FS.\n");
		    }
	    else if (strcmp (encrypted_fs_mode, "off") == 0)
		    {
		      encrypted_fs_data.mode = MODE_ENCRYPTED_FS_DISABLED;
		      ui_print ("Disabling Encrypted FS.\n");
		    }
	    else
		    {
		      ui_print ("Error: invalid Encrypted FS setting.\n");
		      status = INSTALL_ERROR;
		    }
	     
	      // Recovery strategy: if the data partition is damaged, disable encrypted file systems.
	      // This preventsthe device recycling endlessly in recovery mode.
	      if ((encrypted_fs_data.mode == MODE_ENCRYPTED_FS_ENABLED) && 
		  (read_encrypted_fs_info (&encrypted_fs_data)))
		    {
		      ui_print
			("Encrypted FS change aborted, resetting to disabled state.\n");
		      encrypted_fs_data.mode = MODE_ENCRYPTED_FS_DISABLED;
		    }
	     if (status != INSTALL_ERROR)
		    {
		      if (erase_volume ("/data"))
			      {
				ui_print ("Data wipe failed.\n");
				status = INSTALL_ERROR;
			      }
		      else if (erase_volume ("/cache"))
			      {
				ui_print ("Cache wipe failed.\n");
				status = INSTALL_ERROR;
			      }
		      else
			if ((encrypted_fs_data.mode ==
			     MODE_ENCRYPTED_FS_ENABLED)
			    &&
			    (restore_encrypted_fs_info (&encrypted_fs_data)))
			      {
				ui_print ("Encrypted FS change aborted.\n");
				status = INSTALL_ERROR;
			      }
		      else
			      {
				ui_print
				  ("Successfully updated Encrypted FS.\n");
				status = INSTALL_SUCCESS;
			      }
		    }
	  }
  else if (update_package != NULL)
	  {
	    status = install_package (update_package);
	    if (status != INSTALL_SUCCESS)
	      ui_print ("Installation aborted.\n");
	  }
  else if (wipe_data)
	  {
	    if (device_wipe_data ())
	      status = INSTALL_ERROR;
	    if (erase_volume ("/data"))
	      status = INSTALL_ERROR;
	    if (wipe_cache && erase_volume ("/cache"))
	      status = INSTALL_ERROR;
	    if (status != INSTALL_SUCCESS)
	      ui_print ("Data wipe failed.\n");
	  }
  else if (wipe_cache)
	  {
	    if (wipe_cache && erase_volume ("/cache"))
	      status = INSTALL_ERROR;
	    if (status != INSTALL_SUCCESS)
	      ui_print ("Cache wipe failed.\n");
	  }
  else
	  {
	    status = INSTALL_ERROR;	// No command specified
	  }
   if (status != INSTALL_SUCCESS)
    ui_set_background (BACKGROUND_ICON_RZ);
  if (status != INSTALL_SUCCESS /*|| ui_text_visible() */ )
	  {
	    prompt_and_wait ();
	  }
   
    // Otherwise, get ready to boot the main system...
    finish_recovery (send_intent);
  ui_print ("Rebooting...\n");
  sync ();
  reboot (RB_AUTOBOOT);
  return EXIT_SUCCESS;
}

 void
reboot_android ()
{
  ui_print ("\n-- Rebooting into android --\n");
  ensure_path_mounted ("/system");
  remove ("/system/recovery_from_boot.p");
  write_files ();
  sync ();
  __reboot (LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
	     LINUX_REBOOT_CMD_RESTART2, NULL);
  
    //or if that doesnt work
    reboot (RB_AUTOBOOT);
} void

reboot_recovery ()
{
  ui_print ("\n-- Rebooting into recovery --\n");
  write_files ();
  sync ();
  __reboot (LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
	     LINUX_REBOOT_CMD_RESTART2, "recovery");
  reboot (RB_AUTOBOOT);
} 

void reboot_bootloader() 
{
	ui_print("\n-- Rebooting into bootloader --\n");
	write_files();
	sync();
	__reboot (LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		LINUX_REBOOT_CMD_RESTART2, "bootloader");
	reboot (RB_AUTOBOOT);
}	

void power_off ()
{
  ui_print ("\n-- Shutting down --");
  write_files ();
  sync ();
  __reboot (LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
	     LINUX_REBOOT_CMD_POWER_OFF, NULL);
}  void

ui_printf_int (const char *format, int arg) 
{
  char *out = calloc (strlen (format) + 12, sizeof (char));

  sprintf (out, format, arg);
  ui_print ("%s", out);
  free (out);
}   void

get_check_menu_opts (char **items, char **chk_items, int *flags) 
{
  int i;
  int count;

  for (count = 0; chk_items[count]; count++);	// count it
  for (i = 0; i < count; i++)
	  {
	    if (items[i] != NULL)
	      free (items[i]);
	    items[i] = calloc (strlen (chk_items[i]) + 5, sizeof (char));	// 4 for "(*) " and 1 for the NULL-terminator
	    sprintf (items[i], "(%s) %s", (*flags & (1 << i)) ? "*" : " ",
		     chk_items[i]);
} }  void

show_check_menu (char **headers, char **chk_items, int *flags) 
{
  int chosen_item = -1;
   int i;

  for (i = 0; chk_items[i]; i++);	// count it
  i += 2;			// i is the index of NULL in chk_items[], and we want an array 1 larger than chk_items, so add 1 for that and 1 for the NULL at the end => i+2
  char **items = calloc (i, sizeof (char *));

  items[0] = "Finished";
  items[i] = NULL;
   while (chosen_item != 0)
	  {
	    get_check_menu_opts (items + 1, chk_items, flags);	// items+1 so we don't overwrite the Finished option
	    chosen_item =
	      get_menu_selection (headers, items, 0,
				  chosen_item == -1 ? 0 : chosen_item);
	    if (chosen_item == 0)
		    {
		      continue;
		    }
	    chosen_item -= 1;	// make it 0-indexed for bit-flipping the int
	    *flags ^= (1 << chosen_item);	// flip the bit corresponding to the chosen item
	    chosen_item += 1;	// don't ruin the loop!
	  }
}

 int
runve (char *filename, char **argv, char **envp, int secs) 
{
  int opipe[2];
  int ipipe[2];

  pipe (opipe);
  pipe (ipipe);
  pid_t pid = fork ();
  int status = 0;

  if (pid == 0)
	  {
	    dup2 (opipe[1], 1);
	    dup2 (ipipe[0], 0);
	    close (opipe[0]);
	    close (ipipe[1]);
	    execve (filename, argv, envp);
	    char *error_msg = calloc (strlen (filename) + 20, sizeof (char));

	    sprintf (error_msg, "Could not execute %s\n", filename);
	    ui_print ("%s", error_msg);
	    free (error_msg);
	    return (9999);
	  }
  close (opipe[1]);
  close (ipipe[0]);
  FILE * from = fdopen (opipe[0], "r");
  FILE * to = fdopen (ipipe[1], "w");
  char *cur_line = calloc (100, sizeof (char));
  char *tok;
  int total_lines;
  int cur_lines;
  int num_items;
  int num_headers;
  int num_chks;
  float cur_progress;
  char **items = NULL;
  char **headers = NULL;
  char **chks = NULL;
   int i = 0;			// iterator for menu items
  int j = 0;			// iterator for menu headers
  int k = 0;			// iterator for check menu items
  int l = 0;			// iterator for outputting flags from check menu
  int flags = INT_MAX;
  int choice;

   while (fgets (cur_line, 100, from) != NULL)
	  {
	    printf ("%s", cur_line);
	    tok = strtok (cur_line, " \n");
	    if (tok == NULL)
		    {
		      continue;
		    }
	    if (strcmp (tok, "*") == 0)
		    {
		      tok = strtok (NULL, " \n");
		      if (tok == NULL)
			      {
				continue;
			      }
		      if (strcmp (tok, "ptotal") == 0)
			      {
				ui_set_progress (0.0);
				ui_show_progress (1.0, 0);
				total_lines = atoi (strtok (NULL, " "));
			      }
		      else if (strcmp (tok, "print") == 0)
			      {
				ui_print ("%s", strtok (NULL, ""));
			      }
		      else if (strcmp (tok, "items") == 0)
			      {
				num_items = atoi (strtok (NULL, " \n"));
				if (items != NULL)
				  free (items);
				items =
				  calloc ((num_items + 1), sizeof (char *));
				items[num_items] = NULL;
				i = 0;
			      }
		      else if (strcmp (tok, "item") == 0)
			      {
				if (i < num_items)
					{
					  tok = strtok (NULL, "\n");
					  items[i] =
					    calloc ((strlen (tok) + 1),
						    sizeof (char));
					  strcpy (items[i], tok);
					  i++;
					}
			      }
		      else if (strcmp (tok, "headers") == 0)
			      {
				num_headers = atoi (strtok (NULL, " \n"));
				if (headers != NULL)
				  free (headers);
				headers =
				  calloc ((num_headers + 1), sizeof (char *));
				headers[num_headers] = NULL;
				j = 0;
			      }
		      else if (strcmp (tok, "header") == 0)
			      {
				if (j < num_headers)
					{
					  tok = strtok (NULL, "\n");
					  if (tok)
						  {
						    headers[j] =
						      calloc ((strlen (tok) +
							       1),
							      sizeof (char));
						    strcpy (headers[j], tok);
						  }
					  else
						  {
						    headers[j] = "";
						  }
					  j++;
					}
			      }
		      else if (strcmp (tok, "show_menu") == 0)
			      {
				choice =
				  get_menu_selection (headers, items, 0, 0);
				fprintf (to, "%d\n", choice);
				fflush (to);
			      }
		      else if (strcmp (tok, "pcur") == 0)
			      {
				cur_lines = atoi (strtok (NULL, "\n"));
				if (cur_lines % 10 == 0
				     || total_lines - cur_lines < 10)
					{
					  cur_progress =
					    (float) cur_lines /
					    ((float) total_lines);
					  ui_set_progress (cur_progress);
					}
				if (cur_lines == total_lines)
				  ui_reset_progress ();
			      }
		      else if (strcmp (tok, "check_items") == 0)
			      {
				num_chks = atoi (strtok (NULL, " \n"));
				if (chks != NULL)
				  free (chks);
				chks =
				  calloc ((num_chks + 1), sizeof (char *));
				chks[num_chks] = NULL;
				k = 0;
			      }
		      else if (strcmp (tok, "check_item") == 0)
			      {
				if (k < num_chks)
					{
					  tok = strtok (NULL, "\n");
					  chks[k] =
					    calloc (strlen (tok) + 1,
						    sizeof (char));
					  strcpy (chks[k], tok);
					  k++;
					}
			      }
		      else if (strcmp (tok, "show_check_menu") == 0)
			      {
				show_check_menu (headers, chks, &flags);
				for (l = 0; l < num_chks; l++)
					{
					  fprintf (to, "%d\n",
						    ! !(flags & (1 << l)));
					}
				fflush (to);
			      }
		      else if (strcmp (tok, "show_indeterminate_progress") ==
			       0)
			      {
				ui_show_indeterminate_progress ();
			      }
		      else
			      {
				ui_print ("unrecognized command ");
				ui_print ("%s", tok);
				ui_print ("\n");
			      }
		    }
	  }
   while (waitpid (pid, &status, 0) == 0)
	  {
	    sleep (1);
	  }
  ui_print ("\n");
  free (cur_line);
  return status;
}


