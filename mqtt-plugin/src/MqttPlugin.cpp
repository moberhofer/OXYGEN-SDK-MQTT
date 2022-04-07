#include "configuration/Configuration.h"
#include "Service.h"
#include "Utility.h"
#include "Types.h"

//
#include "odkfw_properties.h"
#include "odkfw_custom_request_handler.h"
#include "odkfw_software_channel_plugin.h"
#include "odkfw_resampler.h"

#include "qml.rcc.h"

//
#include "fmt/core.h"

using namespace odk::framework;

// Manifest constains necessary metadata for oxygen plugins
//   OxygenPlugin.name: unique plugin identifier; please use your (company) name to avoid name conflicts.
//   This name is also used as a prefix in all custom config item keys.
//   OxygenPlugin.uuid: unique number (generated by a GUID/UUID generator tool)
//   that stored in configuration files to match channels etc. to the correct plugin
static const char *PLUGIN_MANIFEST =
    R"XML(<?xml version="1.0"?> 
                       
<OxygenPlugin name="MQTT_PLUGIN" version="1.0" uuid="45260878-16ac-4b92-a865-a10ccfb97f7c">
  <Info name="MQTT Plugin: Connect Oxygen to MQTT Brokers.">
    <Vendor name="Dewetron/KAI"/>
    <Description>
    A plugin to send and receive MQTT messages from a broker.
    </Description>
  </Info>
  <Host minimum_version="6.0"/>
  <UsesUIExtensions/>
</OxygenPlugin>
)XML";

// A minimal translation file that maps the internal ConfigItem key to a nicer text for the user
static const char *TRANSLATION_EN =
    R"XML(<?xml version="1.0"?>

<!-- English -->
<TS version="2.1" language="en" sourcelanguage="en">

    <!-- Translations for Config-Keys -->
    <context><name>ConfigKeys</name>
        <message><source>MQTT_PLUGIN/ConfigFile</source><translation>Path to config-file.</translation></message>
    </context>
</TS>
)XML";

static const char *MQTT_CONFIG = "MQTT_PLUGIN/ConfigFile";
static const char *MQTT_CONFIG_CACHE = "MQTT_PLUGIN/ConfigFileCache";

class MqttChannel : public SoftwareChannelInstance
{
public:
    MqttChannel()
        : m_config_file_path(new EditableStringProperty("Path to Config-File.")),
          m_config_file_cache(new EditableStringProperty("Internal Config-File Cache"))
    {
        m_config_file_path->setVisiblity("HIDDEN");
        m_config_file_cache->setVisiblity("HIDDEN");
        m_dll_path = getCurrentDllPath();
    }

    ~MqttChannel()
    {
        m_service.disconnect();
    }

    /**
     * @brief Describe how the software channel should be shown in the "Add Channel" dialog
     * @return odk::RegisterSoftwareChannel
     */
    static odk::RegisterSoftwareChannel getSoftwareChannelInfo()
    {
        odk::RegisterSoftwareChannel telegram;
        telegram.m_display_name = "MQTT Plugin: Simply add MQTT to Oxygen.";
        telegram.m_service_name = "CreateChannel";
        telegram.m_display_group = "Data Sources";
        telegram.m_description = "Adds MQTT to Oxygen: Simply subscribe and send channels.";

        telegram.m_ui_item_add = "qml/AddMqttPlugin";
        return telegram;
    }

    /**
     * @brief Create/configure the root channel
     * @param host
     */
    void create(odk::IfHost *host) override
    {
        ODK_UNUSED(host);
        getRootChannel()->setDefaultName(std::string("MQTT")).setDeletable(true).addProperty(MQTT_CONFIG, m_config_file_path).addProperty(MQTT_CONFIG_CACHE, m_config_file_cache);
    }

    /**
     * @brief Called after a new plugin has been created
     * Loads the configuration from file and tries to setup plugin
     * @param params
     * @return InitResult
     */
    InitResult init(const InitParams &params) final
    {
        odk::PropertyList props(params.m_properties);
        std::filesystem::path config_file = props.getString("MQTT_PLUGIN/ConfigFile");

        // Load Configuration
        auto f = plugin::mqtt::config::Configuration::loadFileContent(config_file.u8string());
        if (f.error)
        {
            // TODO: Show error Message?
            return InitResult(false);
        }

        // TODO: Show error Message?
        auto c = m_configuration.load(f.cache);
        if (c.error)
        {
            return InitResult(false);
        }

        // Reflect changes made to JSON config document to a file (e.g. UUIDs)
        const auto cache_path = m_dll_path + "\\" + config_file.filename().u8string() + ".cache";
        plugin::mqtt::config::Configuration::writeToFile(cache_path, c.document);

        // Cache configuration for plugin-reload
        m_config_file_path->setValue(config_file.u8string());
        m_config_file_cache->setValue(c.document.dump());

        return InitResult(createChannelsAndConnect());
    }

    /**
     * @brief Restore plugin from a previous session
     * Trying to recover the plugin from the .cache file if changes have been made during the sessions
     * @param request
     * @param channel_id_map
     * @return true
     * @return false
     */
    bool configure(const odk::UpdateChannelsTelegram &request, std::map<std::uint32_t, std::uint32_t> &channel_id_map) final
    {
        // Find root-channel
        odk::UpdateChannelsTelegram::PluginChannelInfo requested_root;
        bool found = false;
        for (auto &requested_channel : request.m_channels)
        {
            const auto channel_key = requested_channel.m_channel_config.getProperty("SoftwareChannelInstanceKey")->getStringValue();
            if (channel_key == "root")
            {
                requested_root = requested_channel;
                found = true;
                break;
            }
        }

        if (!found)
        {
            return false;
        }

        // Extract properties of interest
        std::filesystem::path config_file;
        std::string cache;
        for (const auto &property : requested_root.m_channel_config.m_properties)
        {
            const auto property_name(property.getName());

            if (property_name == MQTT_CONFIG)
            {
                config_file = property.getStringValue();
            }
            else if (property_name == MQTT_CONFIG_CACHE)
            {
                cache = property.getStringValue();
            }
        }

        // Try to load cache form file, else use cache from previous session
        const auto cache_path = m_dll_path + "\\" + config_file.filename().u8string() + ".cache";
        auto f = plugin::mqtt::config::Configuration::loadFileContent(cache_path);
        if (!f.error)
        {
            cache = f.cache;
        }

        // Load configuration
        auto c = m_configuration.load(cache);
        if (c.error)
        {
            return false;
        }

        if (!f.error)
        {
            // Reflect any changes made during load of configuration to file (e.g. UUIDs)
            plugin::mqtt::config::Configuration::writeToFile(cache_path, c.document);
        }

        // Create channels and connect
        if (!createChannelsAndConnect())
        {
            return false;
        }

        // As keys stay the same, we can now create the channels mapping
        createMappingByKey(request, channel_id_map);

        // Update cache
        if (!f.error)
        {
            m_config_file_cache->setValue(c.document.dump());
        }

        return true;
    }

    /**
     * @brief Create all publish configs and subscriptions handlers and connect to server
     */
    bool createChannelsAndConnect()
    {
        // Create Channels
        createChannels();

        // TODO: If necessary, the plugin could handle more than one
        // Server connection (e.g. for redundancy?), currently, only use the first config
        auto server_configs = m_configuration.getServers();

        if (server_configs.empty())
        {
            return false;
        }
        auto server_config = server_configs[0];

        // Establish the MQTT-Connection - we will simply ignore messages if we are not processing
        m_service.setServerConfiguration(server_config);
        m_service.connect();
        return true;
    }

    /**
     * @brief Create channels (publish and subscribe) and register them in the MQTT-Service
     * @return true
     * @return false
     */
    bool createChannels()
    {
        auto root_channel = getRootChannel();

        // Create all Channels and Groups for the configured subscription
        for (auto topic : m_configuration.getSubscriptions())
        {
            // walk/traverse the output channel map and create the corresponding oxygen output channels
            traverse(topic->getSubscription(), root_channel, topic->getOxygenOutputChannelMap());

            // add the subscription to the MQTT-Service
            m_service.addSubscription(topic->getSubscription());
        }

        // Create configuration for the configured publishers
        auto publishers = m_configuration.getPublishers();

        if (!publishers.empty())
        {
            auto publish_group_channel = addGroupChannel("MQTT@Publish-Group", root_channel);
            publish_group_channel->setDefaultName("Publish-Channels");

            auto used = publish_group_channel->getProperty("Used");
            used->update(odk::Property("Used", false));

            for (auto topic : m_configuration.getPublishers())
            {
                auto publish = topic->getPublisher();
                publish_group_channel->addProperty(publish->getTopic(), publish->getInputChannel());

                m_service.addPublishHandler(publish);
            }
        }
        return true;
    }

    /**
     * @brief Traverse through all subscriptions to create the corresponding Oxygen Output channels
     * @param subscription
     * @param group_channel
     * @param map
     */
    void traverse(const plugin::mqtt::Subscription::Pointer subscription, odk::framework::PluginChannelPtr &group_channel, plugin::mqtt::config::Topic::OxygenOutputChannelMap &map)
    {
        // Iterate channels of the current level
        for (auto channel : map.channels)
        {
            auto &channel_configuration = channel->getConfiguration();
            auto &sampling_configuration = subscription->getSampling();

            // Create a new output channel - using its unique-id as the key
            auto output_channel = addOutputChannel(channel_configuration.uuid, group_channel);
            output_channel->setDefaultName(channel_configuration.name)
                .setDeletable(false);

            const auto range = channel_configuration.range;
            output_channel->setRange(odk::Range(range.min, range.max, range.unit));

            // Set Default properties
            switch (sampling_configuration.mode)
            {
            case plugin::mqtt::SamplingModes::Async:
                output_channel->setSampleFormat(asOdkFormat(sampling_configuration.mode), asOdkFormat(channel_configuration.datatype));
                break;
            case plugin::mqtt::SamplingModes::Sync:
                output_channel->setSampleFormat(asOdkFormat(sampling_configuration.mode), asOdkFormat(channel_configuration.datatype))
                    .setSimpleTimebase(sampling_configuration.sample_rate.value());

                output_channel->setSamplerate({sampling_configuration.sample_rate.value(), "Hz"});
                break;
            }

            // Link the MQTT-Channel to the Oxygen output channel using its local id
            channel_configuration.local_channel_id = output_channel->getLocalId();
        }

        // Create group channels and recursive call traverse
        for (auto &[group_name, sub_map] : map.group_channels)
        {
            auto sub_group_channel = addGroupChannel(group_name, group_channel);
            sub_group_channel->setDefaultName(group_name);

            traverse(subscription, sub_group_channel, sub_map);
        }
    }

    /**
     * @brief Called whenever a property changes
     * Currently, we do not expose any properties where an "on-change" is of interest
     * @return true
     * @return false
     */
    bool update() override
    {
        return true;
    }

    void updatePropertyTypes(const PluginChannelPtr &output_channel) override
    {
        ODK_UNUSED(output_channel);
    }

    void updateStaticPropertyConstraints(const PluginChannelPtr &channel) override
    {
        ODK_UNUSED(channel);
    }

    /**
     * @brief Called by the host to prepare plugin for processing, inform service
     * @param host
     */
    void prepareProcessing(odk::IfHost *host) override
    {
        ODK_UNUSED(host);

        // Use Host Timesource, enable sampling
        m_service.setTimeSource([host](void)
                                {
                                    auto t = getMasterTimestamp(host);   
                                    return plugin::mqtt::Timestamp(
                                        t.m_ticks,
                                        t.m_frequency
                                    ); });
        m_service.prepareProcessing();
    }

    /**
     * @brief Called by the host when plugin shall stop service, stop processing of MQTT messages
     * @param host
     */
    void stopProcessing(odk::IfHost *host) override
    {
        ODK_UNUSED(host);
        m_service.stopProcessing();
    }

    /**
     * @brief Process Subscriptions and append Data to Oxygen Output channels
     * @param context
     * @param host
     */
    void processSubscriptions(ProcessingContext &context, odk::IfHost *host)
    {
        // The service handles multiple subscriptions
        for (auto &subscription : m_service.getSubscriptions())
        {
            auto sampling = subscription->getSampling();

            // A subscription can have multiple channels
            for (auto channel : subscription->getChannels())
            {
                auto samples = channel->getAndClearSamples();
                auto id = channel->getLocalChannelId();
                if (id)
                {
                    // Every channel buffers samples
                    for (auto &sample : samples)
                    {
                        // Handle different datatypes per channel

                        switch (channel->getDatatype())
                        {
                        case plugin::mqtt::Datatype::Integer:
                        {
                            switch (sampling.mode)
                            {
                            case plugin::mqtt::SamplingModes::Async:
                            {
                                int value = sample.pop_back<int>();
                                odk::addSample(host, id.value(), sample.time.ticks, value);
                            }
                            break;
                            case plugin::mqtt::SamplingModes::Sync:
                            {
                                auto values = sample.pop_values<int>();
                                odk::addSamples(host, id.value(), sample.time.ticks, values.data(), sizeof(int) * values.size());
                            }
                            break;
                            }
                        }
                        break;
                        case plugin::mqtt::Datatype::Number:
                        {
                            switch (sampling.mode)
                            {
                            case plugin::mqtt::SamplingModes::Async:
                            {
                                double value = sample.pop_back<double>();
                                odk::addSample(host, id.value(), sample.time.ticks, value);
                            }
                            break;
                            case plugin::mqtt::SamplingModes::Sync:
                            {
                                auto values = sample.pop_values<double>();
                                odk::addSamples(host, id.value(), sample.time.ticks, values.data(), sizeof(double) * values.size());
                            }
                            break;
                            }
                        }
                        break;
                        case plugin::mqtt::Datatype::String:
                        {
                            std::string value = sample.pop_back<std::string>();
                            odk::addSample(host, id.value(), sample.time.ticks, value.c_str(), value.size());
                        }
                        break;
                        }
                    }
                }
            }
        }
    }

    /**
     * @brief Process all Publish-Handlers, get data from selected input channels and send to host
     * @param context
     * @param host
     */
    void processPublishHandlers(ProcessingContext &context, odk::IfHost *host)
    {
        for (auto &publish : m_service.getPublishHandlers())
        {
            // Get Oxygen input-channel for publish-handler
            const auto input_channel_id = publish->getInputChannel()->getValue();
            if (auto input_channel = getInputChannelProxy(input_channel_id))
            {
                const auto timebase = input_channel->getTimeBase();
                const std::uint64_t start_sample = odk::convertTimeToTickAtOrAfter(context.m_window.first, timebase.m_frequency);
                const std::uint64_t end_sample = odk::convertTimeToTickAtOrAfter(context.m_window.second, timebase.m_frequency);

                odk::framework::StreamIterator &iterator = context.m_channel_iterators[input_channel_id];
                iterator.setSkipGaps(false);

                const auto dataformat = input_channel->getDataFormat();
                if (dataformat.m_sample_occurrence == odk::ChannelDataformat::SampleOccurrence::SYNC)
                {
                    const std::size_t num_output_samples = end_sample - start_sample;
                    const auto sample_rate = input_channel->getSampleRate();

                    if (publish->getSampling().mode == plugin::mqtt::SamplingModes::Sync)
                    {
                        std::vector<plugin::mqtt::value_t> values;
                        values.reserve(num_output_samples);

                        for (auto sample_index = start_sample; sample_index < end_sample; ++sample_index)
                        {
                            switch (dataformat.m_sample_format)
                            {
                            case odk::ChannelDataformat::SampleFormat::DOUBLE:
                            {
                                const auto current_value = iterator.value<double>();
                                values.push_back(current_value);
                            }
                            break;

                            case odk::ChannelDataformat::SampleFormat::SINT32:
                            {
                                const auto current_value = iterator.value<int32_t>();
                                values.push_back(current_value);
                            }
                            break;
                            default:
                                // TODO Implement further datatypes?
                                break;
                            }
                            ++iterator;
                        }

                        publish->addSyncSamples(values, sample_rate.m_val);
                    }
                    else
                    {
                        // TODO: Indicate wrong config (sampling modes do not match)
                    }
                }
                else if (dataformat.m_sample_occurrence == odk::ChannelDataformat::SampleOccurrence::ASYNC)
                {
                    if (publish->getSampling().mode == plugin::mqtt::SamplingModes::Async)
                    {
                        while (iterator.valid() && iterator.timestamp() < end_sample)
                        {
                            const uint64_t timestamp = iterator.timestamp();
                            const double timestamp_seconds = timestamp / timebase.m_frequency;

                            switch (dataformat.m_sample_format)
                            {
                            case odk::ChannelDataformat::SampleFormat::DOUBLE:
                            {
                                const auto current_value = iterator.value<double>();
                                publish->addAsyncSample(timestamp_seconds, current_value);
                            }
                            break;

                            case odk::ChannelDataformat::SampleFormat::SINT32:
                            {
                                const auto current_value = iterator.value<int32_t>();
                                publish->addAsyncSample(timestamp, current_value);
                            }
                            break;

                            default:
                                // TODO Implement further datatypes?
                                break;
                            }
                            ++iterator;
                        }
                    }
                    else
                    {
                        // TODO: Indicate wrong config (sampling modes do not match)
                    }
                }
            }
        }

        // Publish data if any
        m_service.publish();
    }

    /**
     * @brief Called by the host to process input and output channels
     * @param context
     * @param host
     */
    void process(ProcessingContext &context, odk::IfHost *host) override
    {
        // Prevent MQTT-Threads from manipulating any buffers while processing
        std::lock_guard<std::mutex> lock(m_service.getLock());
        processSubscriptions(context, host);
        processPublishHandlers(context, host);
    }

private:
    std::shared_ptr<EditableStringProperty> m_config_file_path;
    std::shared_ptr<EditableStringProperty> m_config_file_cache;

    plugin::mqtt::Service m_service;
    plugin::mqtt::config::Configuration m_configuration;
    std::string m_dll_path;
};

class MqttChannelPlugin : public SoftwareChannelPlugin<MqttChannel>
{
public:
    MqttChannelPlugin()
        : m_custom_requests(std::make_shared<odk::framework::CustomRequestHandler>())
    {
        addMessageHandler(m_custom_requests);
    }

    void registerResources() final
    {
        addTranslation(TRANSLATION_EN);
        addQtResources(plugin_resources::QML_RCC_data, plugin_resources::QML_RCC_size);
    }

private:
    std::shared_ptr<odk::framework::CustomRequestHandler> m_custom_requests;
};

OXY_REGISTER_PLUGIN1("MQTT_PLUGIN", PLUGIN_MANIFEST, MqttChannelPlugin);
