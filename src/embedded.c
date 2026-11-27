/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Reuse the upstream config parser without the standalone command-line loop. */
#define main xsystem4_cli_main
#include "system4.c"
#undef main
#include "embedded.h"
#include "modifier.h"
#include <setjmp.h>
#include <pthread.h>

void xsystem4_vm_shutdown(void);
void xsystem4_libraries_destroy(void);
void asset_manager_fini(void);
void audio_end_session(void);
void mixer_fini(void);
void gfx_font_fini(void);
void input_reset(void);

static _Thread_local const struct xsystem4_host *embedded_host;
static _Thread_local jmp_buf embedded_exit;
static _Thread_local int embedded_exit_code;
static _Thread_local int cleanup_stage;
static _Thread_local bool vm_running;

void xsystem4_host_vm_started(void) { vm_running = true; }
void xsystem4_host_vm_closed(void) { vm_running = false; }

void xsystem4_host_game_exit(int code)
{
	if (embedded_host && embedded_host->game_exit)
		embedded_host->game_exit(embedded_host->user, code);
}

_Noreturn void xsystem4_host_exit(int code)
{
	if (!embedded_host)
		pthread_exit(NULL);
	embedded_exit_code = code;
	longjmp(embedded_exit, 1);
}

int xsystem4_host_encoding_override(void)
{
	return embedded_host ? embedded_host->fixed_encoding - 1 : -1;
}

void xsystem4_host_checkpoint(void)
{
	if (embedded_host && cleanup_stage == 0 &&
			embedded_host->checkpoint(embedded_host->user))
		xsystem4_host_exit(0);
}

int xsystem4_host_poll_event(SDL_Event *event)
{
	xsystem4_host_checkpoint();
	return embedded_host->poll_event(embedded_host->user, event);
}

void xsystem4_host_mouse_position(int *x, int *y, int set)
{
	embedded_host->mouse_position(embedded_host->user, x, y, set);
}

void xsystem4_host_text_input(int active)
{
	if (embedded_host && embedded_host->text_input)
		embedded_host->text_input(embedded_host->user, active);
}

const char *xsystem4_host_shader(const char *path)
{
	return embedded_host->shader(embedded_host->user, path);
}

int xsystem4_host_init_graphics(int width, int height)
{
	return embedded_host->init_graphics(embedded_host->user, width, height);
}

unsigned xsystem4_host_framebuffer(void)
{
	return embedded_host->framebuffer(embedded_host->user);
}

void xsystem4_host_present(int width, int height)
{
	embedded_host->present(embedded_host->user, width, height);
	xsystem4_host_checkpoint();
}

static void embedded_error(const char *message)
{
	if (embedded_host && embedded_host->log)
		embedded_host->log(embedded_host->user, message);
}

int xsystem4_host_run(const struct xsystem4_host *host, const char *game,
		const char *save_directory, const char *font, const char *encoding)
{
	embedded_host = host;
	embedded_exit_code = 0;
	cleanup_stage = 0;
	vm_running = false;
	sys_error_handler = embedded_error;
	sys_exit_handler = xsystem4_host_exit;
	set_utf8_release(false);
	sys4_set_text_gbk(false);
	if (!setjmp(embedded_exit)) {
		if (!config_init_with_dir(game))
			ERROR("No System40.ini or AliceStart.ini in %s", game);
		if (config.view_width < 1 || config.view_height < 1 ||
				config.view_width > 8192 || config.view_height > 8192)
			ERROR("Invalid game resolution");
		free(config.home_dir);
		config.home_dir = xstrdup(save_directory);
		read_user_config();
		if (encoding && *encoding)
			sys4_set_text_gbk(!strcasecmp(encoding, "GBK"));
		free(config.save_dir);
		config.save_dir = xstrdup(save_directory);
		if (font && *font) {
			for (int i = 0; i < 2; i++) {
				if (!config.font_paths[i]) config.font_paths[i] = xstrdup(font);
			}
		}
		int error;
		char *path = resolve_configured_ain_path(NULL);
		if (!path) ERROR("AIN script does not exist: %s", config.ain_filename);
		config.ain_basename_utf8 = xstrdup(path_basename(path));
		ain = ain_open(path, &error);
		if (ain && !sys4_source_is_utf8 && ain_content_is_utf8(ain)) {
			ain_free(ain);
			set_utf8_release(true);
			ain = ain_open(path, &error);
		}
		free(path);
		if (!ain) ERROR("%s", ain_strerror(error));
		mkdir_p(config.save_dir);
		apply_game_specific_hacks(ain);
		init_mai_word_table();
		init_text_dict();
		if (config.msgskip_delay) set_msgskip_delay(ain, config.msgskip_delay);
		asset_manager_init();
		embedded_exit_code = vm_execute_ain(ain);
		xsystem4_host_game_exit(embedded_exit_code);
	}
	/* Advance before each cleanup: a fatal module finalizer must not recurse. */
	while (cleanup_stage < 8) {
		switch (cleanup_stage++) {
		case 0:
			if (vm_running) { vm_running = false; xsystem4_vm_shutdown(); }
			break;
		case 1: audio_end_session(); mixer_fini(); break;
		case 2: xsystem4_libraries_destroy(); break;
		case 3: gfx_font_fini(); break;
		case 4: gfx_fini(); break;
		case 5: asset_manager_fini(); break;
		case 6: if (ain) ain_free(ain); ain = NULL; break;
		case 7:
			modifier_end_session();
			sys4_reset_text_dict();
			sys4_reset_mai_word_table();
			reset_config();
			input_reset();
			break;
		}
	}
	embedded_host = NULL;
	sys_error_handler = NULL;
	sys_exit_handler = NULL;
	return embedded_exit_code;
}
