#include "SessionManager.h"
#include "TrackProcessor.h"
#include "ActionHelper.h"
#include"PluginManager.h"

void SessionManager::newSession (MainComponent* mc)
{
    auto session = mc->getSessionFile();
    if (! session.exists())
    {
        if (! mc->getTracks().isEmpty())
        {
            int areYouSure = NativeMessageBox::showOkCancelBox (AlertWindow::AlertIconType::WarningIcon, String ("Unsaved Session"),
                String ("This session has not yet been saved. This action will cause you work to be lost. Are you sure you want to do this?"),
                nullptr, nullptr);

            switch (areYouSure)
            {
            case 0: //cancel
                return;
            case 1: //ok
                break;
            }
        }
    }
    else
    {
        saveSession (mc);
    }
    clearTracks (mc, mc->getTracks());

    mc->setSessionFile (File());
    mc->getUndoManager().clearUndoHistory();
}

void SessionManager::openSession (MainComponent* mc, const File* sessionFile)
{
#if JUCE_ANDROID
    return; //@TODO Figure out file browser for android
#endif

    newSession (mc);

    if (sessionFile == nullptr)
    {
        FileChooser nativeFileChooser (String ("Open Session"), {}, "*.chow", true);

        if (nativeFileChooser.browseForFileToOpen())
        {
            sessionFile = new File (nativeFileChooser.getResult());
        }
        else
            return;
    }
        
    std::unique_ptr<XmlElement> sessionXml (parseXML (*sessionFile));
    if (sessionXml.get() != nullptr)
    {
        XmlElement* tracksXml = sessionXml->getChildByName ("Tracks");
        if (tracksXml != nullptr)
        {
            XmlElement* trackXml (tracksXml->getFirstChildElement());
            while (trackXml != nullptr)
            {
                parseTrackXml (mc, trackXml);
                trackXml = trackXml->getNextElement();
            }
        }

        mc->setSessionFile (sessionFile->getParentDirectory());
    }
}

void SessionManager::saveSession (MainComponent* mc)
{
    auto session = mc->getSessionFile();
    if (! session.exists())
    {
        saveSessionAs (mc);
        return;
    }
    File stemsFolder = session.getChildFile ("Stems");
    
    std::unique_ptr<XmlElement> xml (new XmlElement ("NewMixerSession"));
    xml->setAttribute ("SessionName", session.getFileName());

    std::unique_ptr<XmlElement> xmlTracks (new XmlElement ("Tracks"));
    auto& tracks = mc->getTracks();
    copyTrackFiles (mc, tracks, stemsFolder);
    saveTracksToXml (tracks, xmlTracks.get());
    xml->addChildElement (xmlTracks.release());

    File saveFile (session.getChildFile (session.getFileName() + ".chow"));
    xml->writeTo (saveFile, {});
}

void SessionManager::saveSessionAs (MainComponent* mc, File* sessionFolder)
{
#if JUCE_ANDROID
    return;
#endif

    if (sessionFolder == nullptr)
    {
        FileChooser nativeFileSaver (String ("Save Session As"), {}, {}, true);

        if (nativeFileSaver.browseForFileToSave (true))
        {
            sessionFolder = new File (nativeFileSaver.getResult().withFileExtension ({}));
        }
        else
            return;
    }

    if (! sessionFolder->exists())
    {
        sessionFolder->createDirectory();
        
        File stemsFolder (sessionFolder->getChildFile ("Stems"));
        stemsFolder.createDirectory();

        mc->setSessionFile (*sessionFolder);
        saveSession (mc);
    }
}

void SessionManager::clearTracks (MainComponent* mc, OwnedArray<Track>& tracks)
{
    while (! tracks.isEmpty())
    {
        ActionHelper::changeSelect (mc, true);
        ActionHelper::deleteSelectedTrack (mc);
    }
}

void SessionManager::parseTrackXml (MainComponent* mc, XmlElement* trackXml)
{
    File trackFile (trackXml->getStringAttribute ("filePath"));
    if (! validateTrackFile (trackFile))
        return;

    auto* newTrack = new Track (trackFile, "", "", Colour());
    newTrack->getValueTree() = ValueTree::fromXml (*trackXml);

    auto trackX = (int) (trackXml->getDoubleAttribute ("xPos") * mc->getWidth()) + TrackConstants::width / 2;
    auto trackY = (int) (trackXml->getDoubleAttribute ("yPos") * mc->getHeight()) + TrackConstants::width / 2;
    auto diameter = (float) trackXml->getDoubleAttribute ("diameter");
    newTrack->setDiameter (diameter);

    auto mute = trackXml->getBoolAttribute ("mute");
    if (! mute)
        newTrack->toggleMute();

    ActionHelper::addTrack (newTrack, mc, trackX, trackY);

    XmlElement* automationXml (trackXml->getChildByName ("Automation"));
    if (automationXml != nullptr)
    {
        newTrack->getAutoHelper()->setRecorded (true);

        XmlElement* autoValueTreeXML (automationXml->getFirstChildElement());
        if(autoValueTreeXML != nullptr)
            newTrack->getAutoHelper()->getPoints() = ValueTree::fromXml (*autoValueTreeXML);
    }

    XmlElement* pluginListXml (trackXml->getChildByName ("Plugins"));
    if (pluginListXml != nullptr)
    {
        XmlElement* pluginXml (pluginListXml->getFirstChildElement());
        while (pluginXml != nullptr)
        {
            parsePluginXml (newTrack, pluginXml);
            pluginXml = pluginXml->getNextElement();
        }
    }
}

void SessionManager::parsePluginXml (Track* newTrack, XmlElement* pluginXml)
{
    auto& pluginList = PluginManager::getInstance()->getPluginList();

    std::unique_ptr<KnownPluginList::PluginTree> pluginTree (KnownPluginList::createTree (pluginList.getTypes(), KnownPluginList::SortMethod::defaultOrder));
    auto pluginArray = pluginTree->plugins;

    bool found = false;
    for (auto plugin : pluginArray)
    {
        if (plugin.name == pluginXml->getTagName() && plugin.pluginFormatName == pluginXml->getStringAttribute ("Format"))
        {
            found = true;
            newTrack->getProcessor()->getPluginChain()->addPlugin (&plugin);
        }
    }

    if (found)
    {
        PropertySet pluginState;
        pluginState.restoreFromXml (*pluginXml->getChildByName ("pluginStateXml"));
        
        MemoryBlock data;
        if (data.fromBase64Encoding (pluginState.getValue ("pluginState")) && data.getSize() > 0)
            newTrack->getProcessor()->getPluginChain()->getPluginList().getLast()->setStateInformation (data.getData(), (int) data.getSize());
    }
    else
    {
        NativeMessageBox::showMessageBox (AlertWindow::AlertIconType::WarningIcon, String ("Plugin not found!"),
                                          String ("The following plugin could not be found: " + pluginXml->getTagName()));
    }
}

bool SessionManager::validateTrackFile (File& file)
{
    if (file.existsAsFile())
        return true;

    int errorBox = NativeMessageBox::showYesNoBox (AlertWindow::AlertIconType::WarningIcon, String ("File not found!"),
                                                   String ("The following file could not be found: " + file.getFullPathName() + 
                                                   ".\n Would you like to locate this file?"), nullptr, nullptr);

    switch (errorBox)
    {
    case 0: //"No"
        return false;
    case 1: //"Yes"
        FileChooser nativeFileChooser (String ("Locate File"), {}, "*.wav", true);

        if (nativeFileChooser.browseForFileToOpen())
        {
            file = File (nativeFileChooser.getResult());
            return true;
        }
    }
    return false;
}

void SessionManager::copyTrackFiles (MainComponent* mc, OwnedArray<Track>& tracks, const File stemsFolder)
{
    Array<Track*> copyTracks;
    Array<int> x;
    Array<int> y;

    for (const auto track : tracks)
    {
        File trackFile (track->getFilePath());
        if (trackFile.getParentDirectory() != stemsFolder)
        {
            File copyTrackFile = stemsFolder.getNonexistentChildFile (trackFile.getFileNameWithoutExtension(), trackFile.getFileExtension());
            trackFile.copyFileTo (copyTrackFile);

            String trackName (track->getName());
            String trackShortName (track->getShortName());
            Colour trackColour = track->getColour();
            auto* newTrack = new Track (copyTrackFile, trackName, trackShortName, trackColour);

            auto trackX = track->getBoundsInParent().getX() + TrackConstants::width / 2;
            auto trackY = track->getBoundsInParent().getY() + TrackConstants::width / 2;
            auto diameter = track->getDiameter();
            newTrack->setDiameter (diameter);

            if (! track->getProcessor()->getIsMute())
                newTrack->toggleMute();

            newTrack->getAutoHelper()->setRecorded (track->getAutoHelper()->isRecorded());
            newTrack->getAutoHelper()->getPoints() = track->getAutoHelper()->getPoints().createCopy();

            newTrack->getProcessor()->setPluginChain (track->getProcessor()->getPluginChain());

            copyTracks.add (newTrack);
            x.add (trackX);
            y.add (trackY);
        }
        else
        {
            copyTracks.add (new Track (*track));
            x.add (track->getBoundsInParent().getX() + TrackConstants::width / 2);
            y.add (track->getBoundsInParent().getY() + TrackConstants::width / 2);
        }
    }
    clearTracks(mc, tracks);

    for (int i = 0; i < copyTracks.size(); i++)
        ActionHelper::addTrack (copyTracks[i], mc, x[i], y[i]);
}

void SessionManager::saveTracksToXml (const OwnedArray<Track>& tracks, XmlElement* xmlTracks)
{
    for (auto track : tracks)
    {
        auto xmlTrack = track->getValueTree().createXml();
        
        xmlTrack->setAttribute ("xPos", track->getRelX());
        xmlTrack->setAttribute ("yPos", track->getRelY());
        xmlTrack->setAttribute ("diameter", track->getDiameter());
        xmlTrack->setAttribute ("mute", track->getProcessor()->getIsMute());

        xmlTrack->setAttribute ("filePath", track->getFilePath());

        saveAutomationToXml (track, xmlTrack.get());
        savePluginsToXml (track, xmlTrack.get());

        xmlTracks->addChildElement (xmlTrack.release());
    }
}

void SessionManager::saveAutomationToXml (Track* track, XmlElement* xmlTrack)
{
    const auto trackAutomation = track->getAutoHelper();
    if (trackAutomation->isRecorded())
    {
        std::unique_ptr<XmlElement> xmlAutomation (new XmlElement ("Automation"));
        auto autoValueTreeXML = track->getAutoHelper()->getPoints().createXml();
        xmlAutomation->addChildElement (autoValueTreeXML.release());
        xmlTrack->addChildElement (xmlAutomation.release());
    }
}

void SessionManager::savePluginsToXml (Track* track, XmlElement* xmlTrack)
{
    const auto pluginChain = track->getProcessor()->getPluginChain();
    if (pluginChain->getNumPlugins() > 0)
    {
        std::unique_ptr<XmlElement> xmlPlugins (new XmlElement ("Plugins"));

        for (auto plugin : pluginChain->getPluginList())
        {
            std::unique_ptr<XmlElement> xmlPluginInstance (new XmlElement (plugin->getName()));
            xmlPluginInstance->setAttribute ("Format", plugin->getPluginDescription().pluginFormatName);

            MemoryBlock data;
            plugin->getStateInformation (data);

            PropertySet pluginState;
            pluginState.setValue ("pluginState", data.toBase64Encoding());

            std::unique_ptr<XmlElement> pluginStateXml (pluginState.createXml ("pluginStateXml"));

            xmlPluginInstance->addChildElement (pluginStateXml.release());
            xmlPlugins->addChildElement (xmlPluginInstance.release());
        }

        xmlTrack->addChildElement (xmlPlugins.release());
    }
}
