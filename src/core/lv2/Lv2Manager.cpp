/*
 * Lv2Manager.cpp - Implementation of Lv2Manager class
 *
 * Copyright (c) 2018-2020 Johannes Lorenz <jlsf2013$users.sourceforge.net, $=@>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include "Lv2Manager.h"

#ifdef LMMS_HAVE_LV2

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <lilv/lilv.h>
#include <lv2.h>
#include <lv2/lv2plug.in/ns/ext/buf-size/buf-size.h>
#include <lv2/lv2plug.in/ns/ext/options/options.h>
#include <QDebug>
#include <QDir>
#include <QLibrary>
#include <QElapsedTimer>

#include "ConfigManager.h"
#include "Engine.h"
#include "Plugin.h"
#include "PluginFactory.h"
#include "Lv2ControlBase.h"
#include "Lv2Options.h"
#include "PluginIssue.h"




const std::set<const char*, Lv2Manager::CmpStr> Lv2Manager::pluginBlacklist =
{
	// github.com/calf-studio-gear/calf, #278
	"http://calf.sourceforge.net/plugins/Analyzer",
	"http://calf.sourceforge.net/plugins/BassEnhancer",
	"http://calf.sourceforge.net/plugins/CompensationDelay",
	"http://calf.sourceforge.net/plugins/Crusher",
	"http://calf.sourceforge.net/plugins/Exciter",
	"http://calf.sourceforge.net/plugins/Saturator",
	"http://calf.sourceforge.net/plugins/StereoTools",
	"http://calf.sourceforge.net/plugins/TapeSimulator",
	"http://calf.sourceforge.net/plugins/TransientDesigner",
	"http://calf.sourceforge.net/plugins/Vinyl"
};




Lv2Manager::Lv2Manager() :
	m_uridCache(m_uridMap)
{
	const char* dbgStr = getenv("LMMS_LV2_DEBUG");
	m_debug = (dbgStr && *dbgStr);

	m_world = lilv_world_new();
	lilv_world_load_all(m_world);

	m_supportedFeatureURIs.insert(LV2_URID__map);
	m_supportedFeatureURIs.insert(LV2_URID__unmap);
	m_supportedFeatureURIs.insert(LV2_OPTIONS__options);
	// min/max is always passed in the options
	m_supportedFeatureURIs.insert(LV2_BUF_SIZE__boundedBlockLength);
	// block length is only changed initially in AudioEngine CTOR
	m_supportedFeatureURIs.insert(LV2_BUF_SIZE__fixedBlockLength);

	auto supportOpt = [this](Lv2UridCache::Id id)
	{
		Lv2Options::supportOption(uridCache()[id]);
	};
	supportOpt(Lv2UridCache::Id::param_sampleRate);
	supportOpt(Lv2UridCache::Id::bufsz_maxBlockLength);
	supportOpt(Lv2UridCache::Id::bufsz_minBlockLength);
	supportOpt(Lv2UridCache::Id::bufsz_nominalBlockLength);
	supportOpt(Lv2UridCache::Id::bufsz_sequenceSize);
}




Lv2Manager::~Lv2Manager()
{
	lilv_world_free(m_world);
}




AutoLilvNode Lv2Manager::uri(const char *uriStr)
{
	return AutoLilvNode(lilv_new_uri(m_world, uriStr));
}




const LilvPlugin *Lv2Manager::getPlugin(const std::string &uri)
{
	auto itr = m_lv2InfoMap.find(uri);
	return itr == m_lv2InfoMap.end() ? nullptr : itr->second.plugin();
}




const LilvPlugin *Lv2Manager::getPlugin(const QString &uri)
{
	return getPlugin(uri.toStdString());
}




void Lv2Manager::initPlugins()
{
	const LilvPlugins* plugins = lilv_world_get_all_plugins(m_world);
	std::size_t pluginCount = 0, pluginsLoaded = 0;
	QElapsedTimer timer;
	timer.start();

	unsigned blacklisted = 0;
	LILV_FOREACH(plugins, itr, plugins)
	{
		const LilvPlugin* curPlug = lilv_plugins_get(plugins, itr);

		std::vector<PluginIssue> issues;
		Plugin::PluginTypes type = Lv2ControlBase::check(curPlug, issues);
		std::sort(issues.begin(), issues.end());
		auto last = std::unique(issues.begin(), issues.end());
		issues.erase(last, issues.end());
		if (m_debug && issues.size())
		{
			qDebug() << "Lv2 plugin"
				<< qStringFromPluginNode(curPlug, lilv_plugin_get_name)
				<< "(URI:"
				<< lilv_node_as_uri(lilv_plugin_get_uri(curPlug))
				<< ") can not be loaded:";
			for (const PluginIssue& iss : issues) { qDebug() << "  - " << iss; }
		}

		Lv2Info info(curPlug, type, issues.empty());

		m_lv2InfoMap[lilv_node_as_uri(lilv_plugin_get_uri(curPlug))]
			= std::move(info);
		if(issues.empty()) { ++pluginsLoaded; }
		else
		{
			if(std::any_of(issues.begin(), issues.end(),
				[](const PluginIssue& iss) {
				return iss.type() == PluginIssueType::blacklisted; }))
			{
				++blacklisted;
			}
		}
		++pluginCount;
	}

	qDebug() << "Lv2 plugin SUMMARY:"
		<< pluginsLoaded << "of" << pluginCount << " loaded in"
		<< timer.elapsed() << "msecs.";
	if(pluginsLoaded != pluginCount)
	{
		if (m_debug)
		{
			qDebug() <<
				"If you don't want to see all this debug output, please set\n"
				"  environment variable \"LMMS_LV2_DEBUG\" to empty or\n"
				"  do not set it.";
		}
		else
		{
			qDebug() <<
				"For details about not loaded plugins, please set\n"
				"  environment variable \"LMMS_LV2_DEBUG\" to nonempty.";
		}
	}

	// TODO: might be better in the LMMS core
	if(Engine::ignorePluginBlacklist())
	{
		qWarning() <<
			"WARNING! Plugin blacklist disabled! If you want to use the blacklist,\n"
			"  please set environment variable \"LMMS_IGNORE_BLACKLIST\" to empty or\n"
			"  do not set it.";
	}
	else if(blacklisted > 0)
	{
		qDebug() <<
			"Lv2 Plugins blacklisted:" << blacklisted << "of" << pluginCount << "\n"
			"  If you want to ignore the blacklist (dangerous!), please set\n"
			"  environment variable \"LMMS_IGNORE_BLACKLIST\" to nonempty.";
	}
}




bool Lv2Manager::CmpStr::operator()(const char *a, const char *b) const
{
	return std::strcmp(a, b) < 0;
}




bool Lv2Manager::isFeatureSupported(const char *featName) const
{
	return m_supportedFeatureURIs.find(featName) != m_supportedFeatureURIs.end();
}




AutoLilvNodes Lv2Manager::findNodes(const LilvNode *subject,
	const LilvNode *predicate, const LilvNode *object)
{
	return AutoLilvNodes(lilv_world_find_nodes (m_world, subject, predicate, object));
}




// unused + untested yet
bool Lv2Manager::isSubclassOf(const LilvPluginClass* clvss, const char* uriStr)
{
	const LilvPluginClasses* allClasses = lilv_world_get_plugin_classes(m_world);
	const LilvPluginClass* root = lilv_world_get_plugin_class(m_world);
	const LilvPluginClass* search = lilv_plugin_classes_get_by_uri(allClasses,
					uri(uriStr).get());

	auto clssEq = [](const LilvPluginClass* pc1,
		const LilvPluginClass* pc2) -> bool
	{
		return lilv_node_equals(
			lilv_plugin_class_get_uri(pc1),
			lilv_plugin_class_get_uri(pc2));
	};
	bool isFound = false;
	while (!(isFound = clssEq(clvss, search)) && !clssEq(clvss, root))
	{
		clvss = lilv_plugin_classes_get_by_uri(allClasses,
			lilv_plugin_class_get_parent_uri(clvss));
	}
	return isFound;
}




#endif // LMMS_HAVE_LV2
