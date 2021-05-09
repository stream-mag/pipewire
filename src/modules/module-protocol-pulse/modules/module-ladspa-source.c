/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>
#include <spa/utils/json.h>
#include <pipewire/pipewire.h>
#include <pipewire/private.h>

#include "../defs.h"
#include "../module.h"
#include "registry.h"

#define ERROR_RETURN(str)		\
	{				\
		pw_log_error(str);	\
		res = -EINVAL;		\
		goto out;		\
	}

struct module_ladspa_source_data {
	struct module *module;

	struct pw_impl_module *mod;
	struct spa_hook mod_listener;

	struct pw_properties *capture_props;
	struct pw_properties *playback_props;
};

static void module_destroy(void *data)
{
	struct module_ladspa_source_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static void serialize_dict(FILE *f, const struct spa_dict *dict)
{
	const struct spa_dict_item *it;
	spa_dict_for_each(it, dict) {
		size_t len = it->value ? strlen(it->value) : 0;
		fprintf(f, " \"%s\" = ", it->key);
		if (it->value == NULL) {
			fprintf(f, "null");
		} else if ( spa_json_is_null(it->value, len) ||
		    spa_json_is_float(it->value, len) ||
		    spa_json_is_object(it->value, len)) {
			fprintf(f, "%s", it->value);
		} else {
			size_t size = (len+1) * 4;
			char str[size];
				spa_json_encode_string(str, size, it->value);
			fprintf(f, "%s", str);
		}
	}
}

static int module_ladspa_source_load(struct client *client, struct module *module)
{
	struct module_ladspa_source_data *data = module->user_data;
	FILE *f;
	char *args;
	const char *str;
	size_t size;

	pw_properties_setf(data->capture_props, PW_KEY_NODE_GROUP, "ladspa-source-%u", module->idx);
	pw_properties_setf(data->playback_props, PW_KEY_NODE_GROUP, "ladspa-source-%u", module->idx);

	f = open_memstream(&args, &size);
	fprintf(f, "{");
	serialize_dict(f, &module->props->dict);
	fprintf(f, " filter.graph = {");
	fprintf(f, " nodes = [ { ");
	fprintf(f, " type = ladspa ");
	if ((str = pw_properties_get(module->props, "plugin")) == NULL)
		return -EINVAL;
	fprintf(f, " plugin = \"%s\" ", str);
	if ((str = pw_properties_get(module->props, "label")) == NULL)
		return -EINVAL;
	fprintf(f, " label = \"%s\" ", str);
	if ((str = pw_properties_get(module->props, "inputs")) != NULL)
		fprintf(f, " inputs = [ %s ] ", str);
	if ((str = pw_properties_get(module->props, "outputs")) != NULL)
		fprintf(f, " outputs = [ %s ] ", str);
	fprintf(f, " } ] }");
	fprintf(f, " capture.props = {");
	serialize_dict(f, &data->capture_props->dict);
	fprintf(f, " } playback.props = {");
	serialize_dict(f, &data->playback_props->dict);
	fprintf(f, " } }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-filter-chain",
			args, NULL);
	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	pw_log_info("loaded module %p id:%u name:%s", module, module->idx, module->name);
	module_emit_loaded(module, 0);

	return 0;
}

static int module_ladspa_source_unload(struct client *client, struct module *module)
{
	struct module_ladspa_source_data *d = module->user_data;

	pw_log_info("unload module %p id:%u name:%s", module, module->idx, module->name);

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}
	return 0;
}

static const struct module_methods module_ladspa_source_methods = {
	VERSION_MODULE_METHODS,
	.load = module_ladspa_source_load,
	.unload = module_ladspa_source_unload,
};

static const struct spa_dict_item module_ladspa_source_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Virtual LADSPA source" },
	{ PW_KEY_MODULE_USAGE,
		"source_name=<name for the source> "
		"source_properties=<properties for the source> "
		"source_output_properties=<properties for the source output> "
		"master=<name of source to filter> "
		"source_master=<name of source to filter> "
		"format=<sample format> "
		"rate=<sample rate> "
		"channels=<number of channels> "
		"channel_map=<input channel map> "
		"plugin=<ladspa plugin name> "
		"label=<ladspa plugin label> "
		"control=<comma separated list of input control values> "
		"input_ladspaport_map=<comma separated list of input LADSPA port names> "
		"output_ladspaport_map=<comma separated list of output LADSPA port names> "},
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static void position_to_props(struct spa_audio_info_raw *info, struct pw_properties *props)
{
	char *s, *p;
	uint32_t i;

	pw_properties_setf(props, SPA_KEY_AUDIO_CHANNELS, "%u", info->channels);
	p = s = alloca(info->channels * 6);
	for (i = 0; i < info->channels; i++)
		p += snprintf(p, 6, "%s%s", i == 0 ? "" : ",",
				channel_id2name(info->position[i]));
	pw_properties_set(props, SPA_KEY_AUDIO_POSITION, s);
}

struct module *create_module_ladspa_source(struct impl *impl, const char *argument)
{
	struct module *module;
	struct module_ladspa_source_data *d;
	struct pw_properties *props = NULL, *playback_props = NULL, *capture_props = NULL;
	const char *str;
	struct spa_audio_info_raw capture_info = { 0 };
	struct spa_audio_info_raw playback_info = { 0 };
	int res;

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_ladspa_source_info));
	capture_props = pw_properties_new(NULL, NULL);
	playback_props = pw_properties_new(NULL, NULL);
	if (!props || !capture_props || !playback_props) {
		res = -EINVAL;
		goto out;
	}
	if (argument)
		module_args_add_props(props, argument);

	if ((str = pw_properties_get(props, "source_name")) != NULL) {
		pw_properties_set(props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "source_name", NULL);
	} else {
		pw_properties_set(props, PW_KEY_NODE_NAME, "null");
	}
	if ((str = pw_properties_get(props, "source_properties")) != NULL) {
		module_args_add_props(capture_props, str);
		pw_properties_set(props, "source_properties", NULL);
	}
	if (pw_properties_get(playback_props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(playback_props, PW_KEY_MEDIA_CLASS, "Audio/Source");

	if ((str = pw_properties_get(props, "master")) != NULL ||
	    (str = pw_properties_get(props, "source_master")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_NODE_TARGET, str);
		pw_properties_set(props, "master", NULL);
	}

	if (module_args_to_audioinfo(impl, props, &capture_info) < 0) {
		res = -EINVAL;
		goto out;
	}
	playback_info = capture_info;

	position_to_props(&capture_info, capture_props);
	position_to_props(&playback_info, playback_props);

	if (pw_properties_get(capture_props, PW_KEY_NODE_PASSIVE) == NULL)
		pw_properties_set(capture_props, PW_KEY_NODE_PASSIVE, "true");

	module = module_new(impl, &module_ladspa_source_methods, sizeof(*d));
	if (module == NULL) {
		res = -errno;
		goto out;
	}

	module->props = props;
	d = module->user_data;
	d->module = module;
	d->capture_props = capture_props;
	d->playback_props = playback_props;

	return module;
out:
	if (props)
		pw_properties_free(props);
	if (playback_props)
		pw_properties_free(playback_props);
	if (capture_props)
		pw_properties_free(capture_props);
	errno = -res;
	return NULL;
}
