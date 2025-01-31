/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2018
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion_engine
{

Clipboard::Clipboard() {}
Clipboard::~Clipboard() { clearSingletonInstance(); }

JUCE_IMPLEMENT_SINGLETON (Clipboard)

void Clipboard::clear()
{
    if (! isEmpty())
    {
        content.reset();
        broadcaster.sendChangeMessage();
    }
}

void Clipboard::setContent (std::unique_ptr<ContentType> newContent)
{
    if (content != newContent)
    {
        content = std::move (newContent);
        broadcaster.sendChangeMessage();
    }
}

const Clipboard::ContentType* Clipboard::getContent() const     { return content.get(); }
bool Clipboard::isEmpty() const                                 { return content == nullptr; }

void Clipboard::addListener (ChangeListener* l)         { broadcaster.addChangeListener (l); }
void Clipboard::removeListener (ChangeListener* l)      { broadcaster.removeChangeListener (l); }

Clipboard::ContentType::EditPastingOptions::EditPastingOptions (Edit& e, EditInsertPoint& ip, SelectionManager* sm)
    : edit (e), insertPoint (ip), selectionManager (sm)
{}

Clipboard::ContentType::EditPastingOptions::EditPastingOptions (Edit& e, EditInsertPoint& ip)
    : edit (e), insertPoint (ip)
{}

//==============================================================================
Clipboard::ContentType::~ContentType() {}

bool Clipboard::ContentType::pasteIntoEdit (Edit& edit, EditInsertPoint& insertPoint, SelectionManager* sm) const
{
    CRASH_TRACER
    Track::Ptr startTrack;
    double startPos = 0.0;
    insertPoint.chooseInsertPoint (startTrack, startPos, false, sm);

    if (startTrack == nullptr)
    {
        jassertfalse;
        return false;
    }

    Clipboard::ContentType::EditPastingOptions options (edit, insertPoint, sm);
    options.startTrack = startTrack.get();
    options.startTime = startPos;

    return pasteIntoEdit (options);
}

bool Clipboard::ContentType::pasteIntoEdit (const EditPastingOptions&) const    { return false; }


//==============================================================================
//==============================================================================
Clipboard::ProjectItems::ProjectItems() {}
Clipboard::ProjectItems::~ProjectItems() {}

static AudioTrack* getOrInsertAudioTrackNearestIndex (Edit& edit, int trackIndex)
{
    int i = 0;

    // find the next audio track on or after the given index..
    for (auto t : getAllTracks (edit))
    {
        if (i >= trackIndex)
            if (auto at = dynamic_cast<AudioTrack*> (t))
                return at;

        ++i;
    }

    return edit.insertNewAudioTrack (TrackInsertPoint (nullptr, getAllTracks (edit).getLast()), nullptr).get();
}

static double pasteMIDIFileIntoEdit (Edit& edit, const File& midiFile, int& targetTrackIndex,
                                     double startTime, bool importTempoChanges)
{
    CRASH_TRACER
    OwnedArray<MidiList> lists;
    Array<double> tempoChangeBeatNumbers, bpms;
    Array<int> numerators, denominators;

    auto newClipEndTime = startTime;
    double len = 0.0;
    bool importAsNoteExpression = false;

    if (MidiList::looksLikeMPEData (midiFile))
        importAsNoteExpression = edit.engine.getUIBehaviour()
                                    .showOkCancelAlertBox (TRANS("Import as Note Expression?"),
                                                           TRANS("This MIDI file looks like it contains multi-channel MPE data. Do you want to convert this to note expression or import as multiple clips?"),
                                                           TRANS("Convert to Expression"),
                                                           TRANS("Separate Clips"));

    if (MidiList::readSeparateTracksFromFile (midiFile, lists,
                                              tempoChangeBeatNumbers, bpms,
                                              numerators, denominators, len,
                                              importAsNoteExpression))
    {
        auto& tempoSequence = edit.tempoSequence;

        auto startBeat = tempoSequence.timeToBeats (startTime);
        auto endBeat = startBeat + len;

        for (int i = lists.size(); --i >= 0;)
            endBeat = jmax (endBeat, startBeat + lists.getUnchecked (i)->getLastBeatNumber());

        endBeat = startBeat + (std::ceil (endBeat - startBeat));

        if (importTempoChanges)
        {
            if (tempoChangeBeatNumbers.size() > 0)
                tempoSequence.removeTemposBetween (EditTimeRange (startTime, tempoSequence.beatsToTime (endBeat))
                                                     .expanded (0.001), true);

            for (int i = 0; i < tempoChangeBeatNumbers.size(); ++i)
            {
                auto insertTime = tempoSequence.beatsToTime (startBeat + tempoChangeBeatNumbers.getUnchecked (i));
                auto& origTempo = tempoSequence.getTempoAt (insertTime);

                if (std::abs (origTempo.getBpm() - bpms.getUnchecked (i)) > 0.001)
                    if (auto tempo = tempoSequence.insertTempo (insertTime))
                        tempo->setBpm (bpms.getUnchecked (i));

                auto& origTimeSig = tempoSequence.getTimeSigAt (insertTime);

                if (origTimeSig.denominator != denominators.getUnchecked (i)
                    || origTimeSig.numerator != numerators.getUnchecked (i))
                {
                    if (auto timeSig = tempoSequence.insertTimeSig (insertTime))
                        timeSig->setStringTimeSig (String (numerators.getUnchecked (i)) + "/" + String (denominators.getUnchecked (i)));
                }
            }
        }

        auto lastTrackEndTime = Edit::maximumLength;
        --targetTrackIndex;

        for (auto list : lists)
        {
            auto listBeatStart = list->getFirstBeatNumber();
            auto listBeatEnd = jmax (listBeatStart + 1, list->getLastBeatNumber(), endBeat - startBeat);

            if (lastTrackEndTime > listBeatStart)
                ++targetTrackIndex;

            lastTrackEndTime = listBeatEnd;

            ValueTree clipState (IDs::MIDICLIP);
            clipState.setProperty (IDs::channel, list->getMidiChannel(), nullptr);

            if (list->state.isValid())
                clipState.addChild (list->state, -1, nullptr);

            if (auto at = getOrInsertAudioTrackNearestIndex (edit, targetTrackIndex))
            {
                auto time = tempoSequence.beatsToTime ({ startBeat, endBeat });

                if (auto newClip = at->insertClipWithState (clipState, list->getImportedMidiTrackName(), TrackItem::Type::midi,
                                                            { time, 0.0 }, false, false))
                {
                    if (importAsNoteExpression)
                        if (auto mc = dynamic_cast<MidiClip*> (newClip))
                            mc->setMPEMode (true);

                    newClipEndTime = std::max (newClipEndTime, newClip->getPosition().getEnd());
                }
            }
            else
            {
                break;
            }
        }
    }

    return newClipEndTime;
}

struct ProjectItemPastingOptions
{
    bool shouldImportTempoChangesFromMIDI = false;
    bool separateTracks = false;
    bool snapBWavsToOriginalTime = false;
};

static void askUserAboutProjectItemPastingOptions (Engine& engine,
                                                   const Clipboard::ProjectItems& items,
                                                   ProjectItemPastingOptions& options)
{
    auto& pm = engine.getProjectManager();
    auto& ui = engine.getUIBehaviour();

    bool importedMIDIFilesHaveTempoChanges = false;
    int numAudioClips = 0;
    int numAudioClipsWithBWAV = 0;

    for (auto& item : items.itemIDs)
    {
        if (auto source = pm.getProjectItem (item.itemID))
        {
            auto file = source->getSourceFile();

            if (file.exists())
            {
                if (source->isMidi())
                {
                    if (! importedMIDIFilesHaveTempoChanges)
                        importedMIDIFilesHaveTempoChanges = MidiList::fileHasTempoChanges (file);
                }
                else if (source->isWave())
                {
                    ++numAudioClips;

                    if (AudioFile (engine, file).getMetadata()[WavAudioFormat::bwavTimeReference].isNotEmpty())
                        ++numAudioClipsWithBWAV;
                }
            }
        }
    }

    if (importedMIDIFilesHaveTempoChanges)
        options.shouldImportTempoChangesFromMIDI = ui.showOkCancelAlertBox (TRANS("MIDI Clip"),
                                                                            TRANS("Do you want to import tempo and time signature changes from the MIDI clip?"),
                                                                            TRANS("Import"),
                                                                            TRANS("Ignore"));

    if (numAudioClips > 1)
    {
        if (numAudioClipsWithBWAV > 0)
        {
           #if JUCE_MODAL_LOOPS_PERMITTED
            ToggleButton toggle (TRANS("Snap to BWAV"));
            toggle.setSize (200, 20);

            std::unique_ptr<AlertWindow> aw (LookAndFeel::getDefaultLookAndFeel().createAlertWindow (TRANS("Add multiple files"),
                                                                                                     TRANS("Do you want to add multiple files to one track or to separate tracks?"),
                                                                                                     {}, {}, {}, AlertWindow::QuestionIcon, 0, nullptr));

            aw->addCustomComponent (&toggle);
            aw->addButton (TRANS("One track"), 0);
            aw->addButton (TRANS("Separate tracks"), 1);

            options.separateTracks = aw->runModalLoop() == 1;
            options.snapBWavsToOriginalTime = toggle.getToggleState();
           #else
            options.separateTracks = false;
            options.snapBWavsToOriginalTime = false;
           #endif
        }
        else
        {
            options.separateTracks = ! ui.showOkCancelAlertBox (TRANS("Add multiple files"),
                                                                TRANS("Do you want to add multiple files to one track or to separate tracks?"),
                                                                TRANS("One track"),
                                                                TRANS("Separate tracks"));
            options.snapBWavsToOriginalTime = false;
        }
    }
    else if (numAudioClips == 1 && numAudioClipsWithBWAV == 1)
    {
        options.snapBWavsToOriginalTime = ui.showOkCancelAlertBox (TRANS("BWAV Clip"),
                                                                   TRANS("Do you want clip placed at BWAV timestamp or cursor position?"),
                                                                   TRANS("BWAV timestamp"),
                                                                   TRANS("Cursor position"));
    }
}

bool isRecursiveEditClipPaste (const Clipboard::ProjectItems& items, Edit& edit)
{
    auto& pm = edit.engine.getProjectManager();

    for (auto& item : items.itemIDs)
        if (auto source = pm.getProjectItem (item.itemID))
            if (source->isEdit() && source->getID() == edit.getProjectItemID())
                return true;

    return false;
}

bool Clipboard::ProjectItems::pasteIntoEdit (const EditPastingOptions& options) const
{
    auto& e  = options.edit.engine;
    auto& pm = e.getProjectManager();
    auto& ui = options.edit.engine.getUIBehaviour();
    bool anythingPasted = false;

    ProjectItemPastingOptions pastingOptions;

    pastingOptions.separateTracks = options.preferredLayout == FileDragList::vertical;

    if (! options.silent)
        askUserAboutProjectItemPastingOptions (e, *this, pastingOptions);

    if (isRecursiveEditClipPaste (*this, options.edit))
    {
        if (! options.silent)
            ui.showWarningAlert (TRANS("Can't Import Edit"),
                                 TRANS("You can't paste an edit clip into itself!"));

        return false;
    }

    double startTime = 0;
    Track::Ptr insertPointTrack;
    options.insertPoint.chooseInsertPoint (insertPointTrack, startTime, false, options.selectionManager);

    if (insertPointTrack == nullptr)
    {
        jassertfalse;
        return false;
    }

    int targetTrackIndex = insertPointTrack->getIndexInEditTrackList();
    SelectableList itemsAdded;

    for (auto& item : itemIDs)
    {
        if (auto sourceItem = pm.getProjectItem (item.itemID))
        {
            auto file = sourceItem->getSourceFile();
            auto newClipEndTime = startTime;

            if (file.exists())
            {
                if (auto targetTrack = getOrInsertAudioTrackNearestIndex (options.edit, targetTrackIndex))
                {
                    if (sourceItem->isMidi())
                    {
                        newClipEndTime = pasteMIDIFileIntoEdit (options.edit, file, targetTrackIndex, startTime,
                                                                pastingOptions.shouldImportTempoChangesFromMIDI);
                    }
                    else if (sourceItem->isWave())
                    {
                        sourceItem->verifyLength();
                        jassert (sourceItem->getLength() > 0);

                        if (auto newClip = targetTrack->insertWaveClip (sourceItem->getName(), sourceItem->getID(),
                                                                        { { startTime, startTime + sourceItem->getLength() }, 0.0 }, false))
                        {
                            newClipEndTime = newClip->getPosition().getEnd();
                            itemsAdded.add (newClip.get());

                            if (pastingOptions.snapBWavsToOriginalTime)
                                newClip->snapToOriginalBWavTime();
                        }

                    }
                    else if (sourceItem->isEdit())
                    {
                        sourceItem->verifyLength();
                        jassert (sourceItem->getLength() > 0);

                        if (auto newClip = targetTrack->insertEditClip ({ startTime, startTime + sourceItem->getLength() },
                                                                        sourceItem->getID()))
                        {
                            newClipEndTime = newClip->getPosition().getEnd();
                            itemsAdded.add (newClip.get());
                        }
                    }

                    anythingPasted = true;

                    if (pastingOptions.separateTracks)
                        ++targetTrackIndex;
                    else
                        startTime = newClipEndTime;
                }
            }
        }
    }

    if (itemsAdded.isNotEmpty())
        if (auto sm = options.selectionManager)
            sm->select (itemsAdded);

    return anythingPasted;
}

bool Clipboard::ProjectItems::pasteIntoProject (Project& project) const
{
    for (auto& item : itemIDs)
        if (auto source = project.engine.getProjectManager().getProjectItem (item.itemID))
            if (auto newItem = project.createNewItem (source->getSourceFile(),
                                                      source->getType(),
                                                      source->getName(),
                                                      source->getDescription(),
                                                      source->getCategory(),
                                                      true))
                newItem->copyAllPropertiesFrom (*source);

    return ! itemIDs.empty();
}

//==============================================================================
//==============================================================================
Clipboard::Clips::Clips() {}
Clipboard::Clips::~Clips() {}

void Clipboard::Clips::addClip (int trackOffset, const ValueTree& state)
{
    ClipInfo ci;
    ci.trackOffset = trackOffset;
    ci.state = state.createCopy();

    clips.push_back (ci);
}

void Clipboard::Clips::addSelectedClips (const SelectableList& selectedObjects, EditTimeRange range, AutomationLocked automationLocked)
{
    if (range.isEmpty())
        range = Edit::getMaximumEditTimeRange();

    auto selectedClipContents = getClipSelectionWithCollectionClipContents (selectedObjects);

    Clip::Array clipsToPaste;

    for (auto& c : selectedClipContents.getItemsOfType<Clip>())
        if (c->getEditTimeRange().overlaps (range))
            clipsToPaste.add (c);

    if (clipsToPaste.isEmpty())
        return;

    auto& ed = clipsToPaste.getFirst()->edit;

    auto allTracks = getAllTracks (ed);

    auto firstTrackIndex = Edit::maxNumTracks;
    auto overallStartTime = Edit::maximumLength;

    for (auto clip : clipsToPaste)
    {
        overallStartTime = jmin (overallStartTime, jmax (clip->getPosition().getStart(), range.getStart()));
        firstTrackIndex = jmin (firstTrackIndex, jmax (0, allTracks.indexOf (clip->getTrack())));
    }

    for (auto clip : clipsToPaste)
    {
        if (clip->getEditTimeRange().overlaps (range))
        {
            auto clipPos = clip->getPosition();
            auto clippedStart = jmax (clipPos.getStart(), range.getStart());
            auto clippedOffset = clipPos.getOffset() + (clippedStart - clipPos.getStart());
            auto clippedEnd = jmin (clipPos.getEnd(), range.getEnd());

            ClipInfo info;

            clip->flushStateToValueTree();
            info.state = clip->state.createCopy();

            info.state.setProperty (IDs::start, clippedStart - overallStartTime, nullptr);
            info.state.setProperty (IDs::length, clippedEnd - clippedStart, nullptr);
            info.state.setProperty (IDs::offset, clippedOffset, nullptr);

            auto acb = dynamic_cast<AudioClipBase*> (clip);

            if (acb != nullptr && range != Edit::getMaximumEditTimeRange())
            {
                auto inOutPoints = clip->getEditTimeRange().getIntersectionWith (range);
                EditTimeRange fadeIn (clipPos.getStart(), clipPos.getStart() + acb->getFadeIn());
                EditTimeRange fadeOut (clipPos.getEnd() - acb->getFadeOut(), clipPos.getEnd());

                info.state.setProperty (IDs::fadeIn,  fadeIn.overlaps (inOutPoints)  ? fadeIn.getIntersectionWith (inOutPoints).getLength() : 0.0, nullptr);
                info.state.setProperty (IDs::fadeOut, fadeOut.overlaps (inOutPoints) ? fadeOut.getIntersectionWith (inOutPoints).getLength() : 0.0, nullptr);
            }

            info.trackOffset = allTracks.indexOf (clip->getTrack()) - firstTrackIndex;

            if (acb == nullptr || acb->getAutoTempo())
            {
                info.hasBeatTimes = true;

                auto& ts = ed.tempoSequence;
                info.startBeats = ts.timeToBeats (clippedStart) - ts.timeToBeats (overallStartTime);
                info.lengthBeats = ts.timeToBeats (clippedEnd) - ts.timeToBeats (clippedStart);
                info.offsetBeats = ts.getBeatsPerSecondAt (clippedStart) * clippedOffset;
            }

            clips.push_back (info);
        }
    }

    if (automationLocked == AutomationLocked::yes)
        addAutomation (TrackSection::findSections (clipsToPaste), range);
}

void Clipboard::Clips::addAutomation (const juce::Array<TrackSection>& trackSections, EditTimeRange range)
{
    if (range.isEmpty() || trackSections.isEmpty())
        return;
    
    auto allTracks = getAllTracks (trackSections.getFirst().track->edit);
    auto firstTrackIndex = Edit::maxNumTracks;
    auto overallStartTime = Edit::maximumLength;

    for (const auto& trackSection : trackSections)
    {
        overallStartTime = jmin (overallStartTime, jmax (trackSection.range.getStart(), range.getStart()));
        firstTrackIndex = jmin (firstTrackIndex, jmax (0, allTracks.indexOf (trackSection.track)));
    }
    
    for (const auto& trackSection : trackSections)
    {
        for (auto plugin : trackSection.track->pluginList)
        {
            for (int k = 0; k < plugin->getNumAutomatableParameters(); k++)
            {
                auto param = plugin->getAutomatableParameter (k);

                if (param->getCurve().getNumPoints() > 0)
                {
                    AutomationCurveSection section;
                    section.pluginName = plugin->getName();
                    section.paramID = param->paramID;
                    section.trackOffset = jmax (0, allTracks.indexOf (trackSection.track) - firstTrackIndex);
                    section.valueRange = param->getCurve().getValueLimits();

                    const double endTolerence = 0.0001;
                    auto intersection = trackSection.range.getIntersectionWith (range);
                    auto reducedIntersection = intersection.reduced (endTolerence);
                    auto clippedStart = intersection.getStart();
                    auto clippedEnd   = intersection.getEnd();

                    for (int l = 0; l < param->getCurve().getNumPoints(); ++l)
                    {
                        auto pt = param->getCurve().getPoint (l);

                        if (reducedIntersection.containsInclusive (pt.time))
                            section.points.push_back ({ pt.time, pt.value, pt.curve });
                    }

                    if (section.points.empty())
                    {
                        section.points.push_back ({ clippedStart, param->getCurve().getValueAt (clippedStart), 1.0f });
                        section.points.push_back ({ clippedEnd, param->getCurve().getValueAt (clippedEnd), 0.0f });
                    }
                    else
                    {
                        if (section.points[0].time > clippedStart + endTolerence)
                            section.points.insert (section.points.begin(), { clippedStart + endTolerence, param->getCurve().getValueAt (clippedStart + endTolerence), 0.0f });

                        if (section.points[section.points.size() - 1].time < clippedEnd - endTolerence)
                            section.points.push_back ({ clippedEnd - endTolerence, param->getCurve().getValueAt (clippedEnd - endTolerence), 0.0f });
                    }

                    for (auto& p : section.points)
                        p.time -= overallStartTime;

                    std::sort (section.points.begin(), section.points.end());
                    automationCurves.push_back (std::move (section));
                }
            }
        }
    }
}

static void fixClipTimes (ValueTree& state, const Clipboard::Clips::ClipInfo& clip, TempoSequence& tempoSequence, double startOffset)
{
    double start = 0, length = 0, offset = 0;

    if (clip.hasBeatTimes)
    {
        auto offsetInBeats = tempoSequence.timeToBeats (startOffset);
        auto range = tempoSequence.beatsToTime ({ offsetInBeats + clip.startBeats, offsetInBeats + clip.startBeats + clip.lengthBeats });
        start  = range.getStart();
        length = range.getLength();
        offset = clip.offsetBeats / tempoSequence.getBeatsPerSecondAt (start);
    }
    else
    {
        start  = static_cast<double> (state.getProperty (IDs::start)) + startOffset;
        length = state.getProperty (IDs::length);
        offset = state.getProperty (IDs::offset);
    }

    double srcBpm = state[IDs::bpm];   // if clip is coming from preset, it'll have this
                                       // property, so resize it to match tempo
    if (srcBpm > 0)
    {
        auto& destTempo = tempoSequence.getTempoAt (start);
        length = length * srcBpm / destTempo.getBpm();
    }

    state.setProperty (IDs::start, start, nullptr);
    state.setProperty (IDs::length, length, nullptr);
    state.setProperty (IDs::offset, offset, nullptr);

    state.removeProperty (IDs::bpm, nullptr);
    state.removeProperty (IDs::key, nullptr);
}

static bool pastePointsToCurve (const std::vector<AutomationCurve::AutomationPoint>& points, juce::Range<float> valueRange, AutomationCurve& targetCurve, EditTimeRange targetRange)
{
    AutomationCurve newCurve;
    auto dstRange = targetCurve.getValueLimits();
    jassert (! dstRange.isEmpty());

    for (auto p : points)
    {
        if (dstRange != valueRange)
        {
            auto normalised = (p.value - valueRange.getStart()) / valueRange.getLength();
            p.value = dstRange.getStart() + dstRange.getLength() * normalised;
        }

        newCurve.addPoint (p.time, p.value, p.curve);
    }

    if (newCurve.getLength() > 0)
    {
        if (targetRange.isEmpty())
            targetRange = targetRange.withLength (newCurve.getLength());
        else
            newCurve.rescaleAllTimes (targetRange.getLength() / newCurve.getLength());

        targetCurve.mergeOtherCurve (newCurve, targetRange, 0.0, 0.0, false, false);
        return true;
    }

    return false;
}

bool Clipboard::Clips::pasteIntoEdit (const EditPastingOptions& options) const
{
    if (clips.empty())
        return false;

    auto targetTrack = options.startTrack;

    if (targetTrack == nullptr)
    {
        double t;
        options.insertPoint.chooseInsertPoint (targetTrack, t, false, options.selectionManager);
        jassert (targetTrack != nullptr);
    }

    // We can't paste into a folder or submix track, so find the next clip track
    while (targetTrack != nullptr && targetTrack->isFolderTrack())
        targetTrack = targetTrack->getSiblingTrack (1, false);

    if (targetTrack == nullptr)
        return false;

    std::map<EditItemID, EditItemID> remappedIDs;
    SelectableList itemsAdded;

    for (auto& clip : clips)
    {
        auto newClipState = clip.state.createCopy();
        EditItemID::remapIDs (newClipState, nullptr, options.edit, &remappedIDs);
        fixClipTimes (newClipState, clip, options.edit.tempoSequence, options.startTime);

        if (newClipState.hasType (IDs::MARKERCLIP))
        {
            if (auto markerTrack = options.edit.getMarkerTrack())
            {
                if (auto newClip = markerTrack->insertClipWithState (newClipState))
                {
                    itemsAdded.add (newClip);

                    if (auto mc = dynamic_cast<MarkerClip*> (newClip))
                        options.edit.getMarkerManager().checkForDuplicates (*mc, false);
                }
            }
            else
            {
                jassertfalse;
            }
        }
        else if (newClipState.hasType (IDs::CHORDCLIP))
        {
            if (auto chordTrack = options.edit.getChordTrack())
            {
                if (auto newClip = chordTrack->insertClipWithState (newClipState))
                    itemsAdded.add (newClip);
            }
        }
        else if (newClipState.hasType (IDs::ARRANGERCLIP))
        {
            if (auto arrangerTrack = options.edit.getArrangerTrack())
            {
                if (auto newClip = arrangerTrack->insertClipWithState (newClipState))
                    itemsAdded.add (newClip);
            }
        }
        else
        {
            if (auto clipTrack = dynamic_cast<ClipTrack*> (targetTrack->getSiblingTrack (clip.trackOffset, false)))
            {
                if (auto newClip = clipTrack->insertClipWithState (newClipState))
                    itemsAdded.add (newClip);
            }
            else
            {
                jassertfalse;
            }
        }
    }

    for (auto& curve : automationCurves)
    {
        if (! curve.points.empty())
        {
            EditTimeRange destCurveTimeRange (Range<double>::withStartAndLength (options.startTime, 0.0));

            if (auto clipTrack = dynamic_cast<ClipTrack*> (targetTrack->getSiblingTrack (curve.trackOffset, false)))
            {
                for (auto plugin : clipTrack->pluginList)
                {
                    if (plugin->getName() == curve.pluginName)
                    {
                        if (auto targetParam = plugin->getAutomatableParameterByID (curve.paramID))
                        {
                            pastePointsToCurve (curve.points, curve.valueRange, targetParam->getCurve(), destCurveTimeRange);
                            break;
                        }
                    }
                }
            }
            else
            {
                jassertfalse;
            }
        }
    }

    if (itemsAdded.isEmpty())
        return false;

    if (auto sm = options.selectionManager)
        sm->select (itemsAdded);

    if (options.setTransportToEnd && ! options.edit.getTransport().isPlaying())
        options.edit.getTransport().setCurrentPosition (getTimeRangeForSelectedItems (itemsAdded).getEnd());

    return true;
}

bool Clipboard::Clips::pasteIntoEdit (Edit& edit, EditInsertPoint& insertPoint, SelectionManager* sm) const
{
    Clipboard::ContentType::EditPastingOptions options (edit, insertPoint, sm);
    insertPoint.chooseInsertPoint (options.startTrack, options.startTime, false, sm);
    return pasteIntoEdit (options);
}

bool Clipboard::Clips::pasteAfterSelected (Edit& edit, EditInsertPoint& insertPoint, SelectionManager& sm) const
{
    EditPastingOptions options (edit, insertPoint, &sm);
    insertPoint.chooseInsertPoint (options.startTrack, options.startTime, true, &sm);
    return pasteIntoEdit (options);
}

static juce::Array<ClipTrack*> findTracksToInsertInto (Edit& edit, EditInsertPoint& insertPoint, SelectionManager& sm)
{
    auto tracks = sm.getItemsOfType<ClipTrack>();
    bool noFolders = true;

    for (auto ft : sm.getItemsOfType<FolderTrack>())
    {
        for (auto t : ft->getAllAudioSubTracks (true))
            tracks.addIfNotAlreadyThere (t);

        noFolders = false;
    }

    for (auto c : sm.getItemsOfType<Clip>())
    {
        tracks.addIfNotAlreadyThere (c->getClipTrack());
        insertPoint.setNextInsertPoint (edit.getTransport().position, c->getTrack());
    }

    if (tracks.isEmpty() && noFolders)
        tracks.addArray (getClipTracks (edit));

    return tracks;
}

static double getNewClipsTotalLength (const Clipboard::Clips& clips, Edit& edit)
{
    double total = 0;

    for (auto& i : clips.clips)
    {
        auto end = i.hasBeatTimes ? edit.tempoSequence.beatsToTime (i.startBeats + i.lengthBeats)
                                  : (static_cast<double> (i.state.getProperty (IDs::start))
                                       + static_cast<double> (i.state.getProperty (IDs::length)));

        total = std::max (total, end);
    }

    return total;
}

bool Clipboard::Clips::pasteInsertingAtCursorPos (Edit& edit, EditInsertPoint& insertPoint, SelectionManager& sm) const
{
    if (clips.empty())
        return false;

    auto tracks = findTracksToInsertInto (edit, insertPoint, sm);
    auto insertLength = getNewClipsTotalLength (*this, edit);

    if (tracks.isEmpty() || insertLength <= 0)
        return false;

    double cursorPos = edit.getTransport().position;
    auto firstTrackIndex = tracks.getFirst()->getIndexInEditTrackList();

    for (auto t : tracks)
    {
        t->splitAt (cursorPos);
        t->insertSpaceIntoTrack (cursorPos, insertLength);
        firstTrackIndex = std::min (firstTrackIndex, t->getIndexInEditTrackList());
    }

    EditPastingOptions options (edit, insertPoint, &sm);
    options.startTime = cursorPos;
    return pasteIntoEdit (options);
}

//==============================================================================
//==============================================================================
Clipboard::Tracks::Tracks() {}
Clipboard::Tracks::~Tracks() {}

bool Clipboard::Tracks::pasteIntoEdit (const EditPastingOptions& options) const
{
    CRASH_TRACER

    Array<Track::Ptr> newTracks;
    std::map<EditItemID, EditItemID> remappedIDs;

    auto targetTrack = options.startTrack;

    // When pasting tracks, always paste after the selected group of tracks if the target is
    // withing the selection
    auto allTracks = getAllTracks (options.edit);
    if (options.selectionManager != nullptr && options.selectionManager->isSelected (targetTrack.get()))
        for (auto t : options.selectionManager->getItemsOfType<Track>())
            if (allTracks.indexOf (t) > allTracks.indexOf (targetTrack.get()))
                targetTrack = t;

    if (options.selectionManager != nullptr)
        options.selectionManager->deselectAll();

    for (auto& trackState : tracks)
    {
        auto newTrackTree = trackState.createCopy();
        EditItemID::remapIDs (newTrackTree, nullptr, options.edit, &remappedIDs);

        Track::Ptr parentTrack, precedingTrack;

        if (targetTrack != nullptr)
            parentTrack = targetTrack->getParentTrack();

        precedingTrack = targetTrack;

        if (auto newTrack = options.edit.insertTrack (TrackInsertPoint (parentTrack.get(),
                                                                        precedingTrack.get()),
                                                      newTrackTree,
                                                      options.selectionManager))
        {
            newTracks.add (newTrack);

            if (parentTrack == nullptr)
                targetTrack = newTrack;
        }
        else
        {
            break;
        }
    }

    // Find any parameters on the Track that have modifier assignments
    // Check to see if they're assigned to the old modifier IDs
    // If they are, find the new modifier ID equivalents and update them
    // If they can't be found leave them if they're global or a parent of the new track.
    for (auto track : newTracks)
    {
        for (auto param : track->getAllAutomatableParams())
        {
            auto assignments = param->getAssignments();

            for (int i = assignments.size(); --i >= 0;)
            {
                auto ass = assignments.getUnchecked (i);

                // Macro reassignment is done during Plugin::giveNewIDsToPlugins so
                // we need to make sure we don't remove these
                if (dynamic_cast<MacroParameter::Assignment*> (ass.get()) != nullptr)
                    continue;

                const auto oldID = EditItemID::fromProperty (ass->state, IDs::source);
                const auto newID = remappedIDs[oldID];

                if (newID.isValid())
                {
                    newID.setProperty (ass->state, IDs::source, nullptr);
                }
                else
                {
                    // If the modifier is on this track, keep it
                    // If oldID is found in a global track, keep it
                    // If oldID is found in a parent track, keep it
                    if (auto t = getTrackContainingModifier (options.edit, findModifierForID (options.edit, oldID)))
                        if (t == track.get() || TrackList::isFixedTrack (t->state) || track->isAChildOf (*t))
                            continue;

                    // Otherwise remove the assignment
                    param->removeModifier (*ass);
                }
            }
        }
    }

    return true;
}

//==============================================================================
//==============================================================================
Clipboard::TempoChanges::TempoChanges (const TempoSequence& ts, EditTimeRange range)
{
    auto beats = ts.timeToBeats (range);

    double startBeat = std::floor (beats.getStart() + 0.5);
    double endBeat   = std::floor (beats.getEnd() + 0.5);

    bool pointAtStart = false;
    bool pointAtEnd   = false;

    for (auto t : ts.getTempos())
    {
        if (t->startBeatNumber == startBeat) pointAtStart = true;
        if (t->startBeatNumber == endBeat)   pointAtEnd   = true;

        if (range.containsInclusive (t->getStartTime()))
            changes.push_back ({ t->startBeatNumber - startBeat,
                                 t->getBpm(),
                                 t->getCurve() });
    }

    if (! pointAtStart)
        changes.insert (changes.begin(),
                        { startBeat - startBeat,
                          ts.getBpmAt (ts.beatsToTime (startBeat)),
                          ts.getTempoAtBeat (roundToInt (startBeat)).getCurve() });

    if (! pointAtEnd)
        changes.push_back ({ endBeat - startBeat,
                             ts.getBpmAt (ts.beatsToTime (endBeat)),
                             ts.getTempoAtBeat (roundToInt (endBeat)).getCurve() });
}

Clipboard::TempoChanges::~TempoChanges() {}

bool Clipboard::TempoChanges::pasteIntoEdit (const EditPastingOptions& options) const
{
    return pasteTempoSequence (options.edit.tempoSequence, EditTimeRange::emptyRange (options.startTime));
}

bool Clipboard::TempoChanges::pasteTempoSequence (TempoSequence& ts, EditTimeRange targetRange) const
{
    if (changes.empty())
        return false;

    EditTimecodeRemapperSnapshot snap;
    snap.savePreChangeState (ts.edit);

    auto lengthInBeats = changes.back().beat;

    if (targetRange.isEmpty())
        targetRange = targetRange.withEnd (ts.beatsToTime (ts.timeToBeats (targetRange.getStart()) + lengthInBeats));

    auto startBeat = std::floor (ts.timeToBeats (targetRange.getStart()) + 0.5);
    auto endBeat   = std::floor (ts.timeToBeats (targetRange.getEnd())   + 0.5);

    double finalBPM = ts.getBpmAt (ts.beatsToTime (endBeat));
    ts.removeTemposBetween (ts.beatsToTime ({ startBeat, endBeat }), false);
    ts.insertTempo (ts.beatsToTime (startBeat));

    for (auto& tc : changes)
        ts.insertTempo (roundToInt ((tc.beat / lengthInBeats) * (endBeat - startBeat) + startBeat),
                        tc.bpm, tc.curve);

    ts.insertTempo (ts.beatsToTime (endBeat));
    ts.insertTempo (endBeat, finalBPM, 1.0f);

    for (int i = ts.getNumTempos(); --i >= 1;)
    {
        auto tcurr = ts.getTempo (i);
        auto tprev = ts.getTempo (i - 1);

        if (tcurr->startBeatNumber >= startBeat && tcurr->startBeatNumber <= endBeat
              && tcurr->startBeatNumber == tprev->startBeatNumber
              && tcurr->getBpm() == tprev->getBpm())
            ts.removeTempo (i, false);
    }

    snap.remapEdit (ts.edit);
    return true;
}

//==============================================================================
//==============================================================================
class ClipboardTempoTests   : public UnitTest
{
public:
    ClipboardTempoTests() : UnitTest ("ClipboardTempoTests", "Tracktion") {}

    //==============================================================================
    void runTest() override
    {
        runCopyTests();
        runCopyTestsUsingBeatInsertion();
    }

private:
    void expectTempoSetting (TempoSetting& tempo, double bpm, float curve)
    {
        expectWithinAbsoluteError (tempo.getBpm(), bpm, 0.001);
        expectWithinAbsoluteError (tempo.getCurve(), curve, 0.001f);
    }

    void runCopyTests()
    {
        auto edit = Edit::createSingleTrackEdit (*Engine::getEngines()[0]);
        auto& ts = edit->tempoSequence;
        
        beginTest ("Simple copy/paste");
        {
            ts.getTempo (0)->setBpm (120.0);
            
            // N.B. bars start at 0!
            expectEquals (ts.barsBeatsToBeats ({ 0, 0.0 }), 0.0);
            expectEquals (ts.barsBeatsToTime ({ 0, 0.0 }), 0.0);
            expectEquals (ts.barsBeatsToBeats ({ 8, 0.0 }), 32.0);
            expectEquals (ts.barsBeatsToTime ({ 8, 0.0 }), 16.0);

            ts.insertTempo (ts.barsBeatsToBeats ({ 5, 0.0 }), 60.0, 1.0f);
            ts.insertTempo (ts.barsBeatsToBeats ({ 9, 0.0 }), 120.0, 1.0f);

            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 4, 0.0 })), 120.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 6, 0.0 })), 60.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 8, 0.0 })), 60.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 10, 0.0 })), 120.0, 1.0f);
            
            const Range<double> beatRangeToCopy (ts.barsBeatsToBeats ({ 6, 0.0 }), ts.barsBeatsToBeats ({ 8, 0.0 }));
            const auto timeRangeToCopy = ts.beatsToTime (beatRangeToCopy);
            const double numBeatsToInsert = beatRangeToCopy.getLength();

            // Copy tempo changes
            Clipboard::TempoChanges tempoChanges (ts, timeRangeToCopy);
            
            // Insert empty space
            const double timeToInsertAt = ts.barsBeatsToTime ({ 2, 0.0 });
            auto& tempoAtInsertionPoint = ts.getTempoAt (timeToInsertAt);

            const auto beatRangeToInsert = Range<double>::withStartAndLength (ts.timeToBeats (timeToInsertAt), numBeatsToInsert);
            const double lengthInTimeToInsert = ts.beatsToTime (beatRangeToInsert.getLength());
            insertSpaceIntoEdit (*edit, EditTimeRange::withStartAndLength (timeToInsertAt, lengthInTimeToInsert));
            
            const double numBeatsInserted = beatRangeToInsert.getLength();
            const int numBarsInserted = roundToInt (numBeatsInserted / tempoAtInsertionPoint.getMatchingTimeSig().denominator);
            expectWithinAbsoluteError (numBeatsInserted, 8.0, 0.0001);
            expectEquals (numBarsInserted, 2);

            // Ensure tempos are correct at original region
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 4 + numBarsInserted, 0.0 })), 120.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 6 + numBarsInserted, 0.0 })), 60.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 8 + numBarsInserted, 0.0 })), 60.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 10 + numBarsInserted, 0.0 })), 120.0, 1.0f);
            
            // Paste tempo changes
            tempoChanges.pasteTempoSequence (ts, EditTimeRange::withStartAndLength (timeToInsertAt, lengthInTimeToInsert));

            // Ensure tempos are correct at inserted region
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 0, 0.0 })), 120.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 1, 3.0 })), 120.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 2, 0.0 })), 60.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 3, 3.0 })), 60.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 4, 0.0 })), 120.0, 1.0f);
        }
    }
    
    void runCopyTestsUsingBeatInsertion()
    {
        auto edit = Edit::createSingleTrackEdit (*Engine::getEngines()[0]);
        auto& ts = edit->tempoSequence;
        
        beginTest ("Simple copy/paste");
        {
            ts.getTempo (0)->setBpm (120.0);
            
            // N.B. bars start at 0!
            expectEquals (ts.barsBeatsToBeats ({ 0, 0.0 }), 0.0);
            expectEquals (ts.barsBeatsToTime ({ 0, 0.0 }), 0.0);
            expectEquals (ts.barsBeatsToBeats ({ 8, 0.0 }), 32.0);
            expectEquals (ts.barsBeatsToTime ({ 8, 0.0 }), 16.0);

            ts.insertTempo (ts.barsBeatsToBeats ({ 5, 0.0 }), 60.0, 1.0f);
            ts.insertTempo (ts.barsBeatsToBeats ({ 9, 0.0 }), 120.0, 1.0f);

            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 4, 0.0 })), 120.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 6, 0.0 })), 60.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 8, 0.0 })), 60.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 10, 0.0 })), 120.0, 1.0f);
            
            const Range<double> beatRangeToCopy (ts.barsBeatsToBeats ({ 6, 0.0 }), ts.barsBeatsToBeats ({ 8, 0.0 }));
            const auto timeRangeToCopy = ts.beatsToTime (beatRangeToCopy);

            // Copy tempo changes
            Clipboard::TempoChanges tempoChanges (ts, timeRangeToCopy);
            
            // Insert empty space
            const double timeToInsertAt = ts.barsBeatsToTime ({ 2, 0.0 });
            auto& tempoAtInsertionPoint = ts.getTempoAt (timeToInsertAt);
            const auto beatRangeToInsert = beatRangeToCopy.movedToStartAt (ts.timeToBeats (timeToInsertAt));
            insertSpaceIntoEditFromBeatRange (*edit, beatRangeToInsert);
            
            const double numBeatsInserted = beatRangeToInsert.getLength();
            const int numBarsInserted = roundToInt (numBeatsInserted / tempoAtInsertionPoint.getMatchingTimeSig().denominator);
            expectWithinAbsoluteError (numBeatsInserted, 8.0, 0.0001);
            expectEquals (numBarsInserted, 2);

            // Ensure tempos are correct at original region
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 4 + numBarsInserted, 0.0 })), 120.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 6 + numBarsInserted, 0.0 })), 60.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 8 + numBarsInserted, 0.0 })), 60.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 10 + numBarsInserted, 0.0 })), 120.0, 1.0f);
            
            // Paste tempo changes
            tempoChanges.pasteTempoSequence (ts, EditTimeRange (timeToInsertAt, ts.beatsToTime (beatRangeToInsert.getEnd())));

            // Ensure tempos are correct at inserted region
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 0, 0.0 })), 120.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 1, 3.0 })), 120.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 2, 0.0 })), 60.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 3, 3.0 })), 60.0, 1.0f);
            expectTempoSetting (ts.getTempoAtBeat (ts.barsBeatsToBeats ({ 4, 0.0 })), 120.0, 1.0f);
        }
    }
};

static ClipboardTempoTests clipboardTempoTests;


//==============================================================================
//==============================================================================
Clipboard::AutomationPoints::AutomationPoints (const AutomationCurve& curve, EditTimeRange range)
{
    valueRange = curve.getValueLimits();

    bool pointAtStart = false;
    bool pointAtEnd = false;

    for (int i = 0; i < curve.getNumPoints(); ++i)
    {
        auto p = curve.getPoint (i);

        if (p.time == range.getStart())  pointAtStart = true;
        if (p.time == range.getEnd())    pointAtEnd = true;

        if (range.containsInclusive (p.time))
        {
            p.time -= range.getStart();
            points.push_back (p);
        }
    }

    if (! pointAtStart)
        points.insert (points.begin(), AutomationCurve::AutomationPoint (0, curve.getValueAt (range.getStart()), 0));

    if (! pointAtEnd)
        points.push_back (AutomationCurve::AutomationPoint (range.getLength(), curve.getValueAt (range.getEnd()), 0));
}

Clipboard::AutomationPoints::~AutomationPoints() {}

bool Clipboard::AutomationPoints::pasteIntoEdit (const EditPastingOptions&) const
{
    jassertfalse; // TODO: what to do here?
    return false;
}

bool Clipboard::AutomationPoints::pasteAutomationCurve (AutomationCurve& targetCurve, EditTimeRange targetRange) const
{
    return pastePointsToCurve (points, valueRange, targetCurve, targetRange);
}

//==============================================================================
//==============================================================================
Clipboard::MIDIEvents::MIDIEvents() {}
Clipboard::MIDIEvents::~MIDIEvents() {}

std::pair<juce::Array<MidiNote*>, juce::Array<MidiControllerEvent*>> Clipboard::MIDIEvents::pasteIntoClip (MidiClip& clip,
                                                                                                           const juce::Array<MidiNote*>& selectedNotes,
                                                                                                           const juce::Array<MidiControllerEvent*>& selectedEvents,
                                                                                                           double cursorPosition, const std::function<double(double)>& snapBeat,
                                                                                                           int destController) const
{
    auto notesAdded         = pasteNotesIntoClip (clip, selectedNotes, cursorPosition, snapBeat);
    auto controllersAdded   = pasteControllersIntoClip (clip, selectedNotes, selectedEvents, cursorPosition, snapBeat, destController);

    return { notesAdded, controllersAdded };
}

juce::Array<MidiNote*> Clipboard::MIDIEvents::pasteNotesIntoClip (MidiClip& clip, const juce::Array<MidiNote*>& selectedNotes,
                                                                  double cursorPosition, const std::function<double(double)>& snapBeat) const
{
    if (notes.empty())
        return {};

    juce::Array<MidiNote> midiNotes;

    for (auto& n : notes)
        midiNotes.add (MidiNote (n));

    auto beatRange = midiNotes.getReference(0).getRangeBeats();

    for (auto& n : midiNotes)
        beatRange = beatRange.getUnionWith (n.getRangeBeats());

    double insertPos = 0.0;
    if (clip.isLooping())
        insertPos = clip.getContentBeatAtTime (cursorPosition) + clip.getLoopStartBeats();
    else
        insertPos = clip.getContentBeatAtTime (cursorPosition);

    if (! selectedNotes.isEmpty())
    {
        double endOfSelection = 0;

        for (auto* n : selectedNotes)
            endOfSelection = jmax (endOfSelection, n->getEndBeat());

        insertPos = endOfSelection;
    }

    if (clip.isLooping())
    {
        const double offsetBeats = clip.getOffsetInBeats() + clip.getLoopStartBeats();

        if (insertPos - offsetBeats < 0 || insertPos - offsetBeats >= clip.getLoopLengthBeats() - 0.001)
            return {};
    }
    else
    {
        const double offsetBeats = clip.getOffsetInBeats();

        if (insertPos - offsetBeats < 0 || insertPos - offsetBeats >= clip.getLengthInBeats() - 0.001)
            return {};
    }

    double deltaBeats = insertPos - beatRange.getStart();

    if (snapBeat != nullptr)
        deltaBeats = snapBeat (deltaBeats);

    auto& sequence = clip.getSequence();
    auto um = &clip.edit.getUndoManager();
    juce::Array<MidiNote*> notesAdded;

    for (auto& n : midiNotes)
    {
        n.setStartAndLength (n.getStartBeat() + deltaBeats, n.getLengthBeats(), nullptr);

        if (auto note = sequence.addNote (n, um))
            notesAdded.add (note);
    }

    return notesAdded;
}

juce::Array<MidiControllerEvent*> Clipboard::MIDIEvents::pasteControllersIntoClip (MidiClip& clip,
                                                                                   const juce::Array<MidiNote*>& selectedNotes,
                                                                                   const juce::Array<MidiControllerEvent*>& selectedEvents,
                                                                                   double cursorPosition, const std::function<double(double)>& snapBeat,
                                                                                   int destController) const
{
    if (controllers.empty())
        return {};

    juce::Array<MidiControllerEvent> midiEvents;

    for (auto& e : controllers)
        midiEvents.add (MidiControllerEvent (e));

    if (notes.size() > 0)
        destController = -1;

    Array<int> controllerTypes;
    for (auto& e : midiEvents)
        controllerTypes.addIfNotAlreadyThere (e.getType());

    if (controllerTypes.size() > 1)
        destController = -1;

    if (destController != -1)
        for (auto& e : midiEvents)
            e.setType (destController, nullptr);

    auto beatRange = juce::Range<double>::withStartAndLength (midiEvents.getReference(0).getBeatPosition(), 0.0);

    for (auto& e : midiEvents)
        beatRange = beatRange.getUnionWith (juce::Range<double>::withStartAndLength (e.getBeatPosition(), 0.0));

    double insertPos = 0.0;
    if (clip.isLooping())
        insertPos = clip.getContentBeatAtTime (cursorPosition) + clip.getLoopStartBeats();
    else
        insertPos = clip.getContentBeatAtTime (cursorPosition);

    if (! selectedNotes.isEmpty())
    {
        double endOfSelection = 0;

        for (auto* n : selectedNotes)
            endOfSelection = jmax (endOfSelection, n->getEndBeat());

        insertPos = endOfSelection;
    }
    else if (! selectedEvents.isEmpty())
    {
        double endOfSelection = 0;

        for (auto e : selectedEvents)
            endOfSelection = jmax (endOfSelection, e->getBeatPosition());

        insertPos = endOfSelection + 1.0f;
    }

    if (clip.isLooping())
    {
        const double offsetBeats = clip.getOffsetInBeats() + clip.getLoopStartBeats();

        if (insertPos - offsetBeats < 0 || insertPos - offsetBeats >= clip.getLoopLengthBeats() - 0.001)
            return {};
    }
    else
    {
        const double offsetBeats = clip.getOffsetInBeats();

        if (insertPos - offsetBeats < 0 || insertPos - offsetBeats >= clip.getLengthInBeats() - 0.001)
            return {};
    }

    double deltaBeats = insertPos - beatRange.getStart();

    if (snapBeat != nullptr)
        deltaBeats = snapBeat (deltaBeats);

    auto& sequence = clip.getSequence();
    auto um = &clip.edit.getUndoManager();
    juce::Array<MidiControllerEvent*> eventsAdded;

    Array<ValueTree> itemsToRemove;
    for (auto evt : sequence.getControllerEvents())
        if (controllerTypes.contains (evt->getType()) && evt->getBeatPosition() >= beatRange.getStart() + deltaBeats && evt->getBeatPosition() <= beatRange.getEnd() + deltaBeats)
            itemsToRemove.add (evt->state);

    for (auto& v : itemsToRemove)
        sequence.state.removeChild (v, um);

    for (auto& e : midiEvents)
    {
        e.setBeatPosition (e.getBeatPosition() + deltaBeats, um);

        if (auto evt = sequence.addControllerEvent (e, um))
            eventsAdded.add (evt);
    }

    return eventsAdded;
}

bool Clipboard::MIDIEvents::pasteIntoEdit (const EditPastingOptions&) const
{
    return false;
}

//==============================================================================
namespace
{
    static double snapTimeToNearestBeat (Edit& e, double t)
    {
        return TimecodeSnapType::get1BeatSnapType().roundTimeNearest (t, e.tempoSequence);
    }
}

//==============================================================================
//==============================================================================
Clipboard::Pitches::Pitches() {}
Clipboard::Pitches::~Pitches() {}

bool Clipboard::Pitches::pasteIntoEdit (const EditPastingOptions& options) const
{
    if (pitches.empty())
        return false;

    if (options.selectionManager != nullptr)
        options.selectionManager->deselectAll();

    auto startBeat = options.edit.tempoSequence.timeToBeats (snapTimeToNearestBeat (options.edit, options.startTime));
    auto firstPitchBeat = static_cast<double> (pitches.front().getProperty (IDs::startBeat));
    auto offset = startBeat - firstPitchBeat;
    auto um = &options.edit.getUndoManager();

    for (auto& state : pitches)
    {
        auto time = options.edit.tempoSequence.beatsToTime (offset + static_cast<double> (state.getProperty (IDs::startBeat)));

        if (auto pitch = options.edit.pitchSequence.insertPitch (time))
        {
            jassert (pitch->state.getNumChildren() == 0); // this would need handling

            copyValueTreeProperties (pitch->state, state, um,
                                     [] (const juce::Identifier& name) { return name != IDs::startBeat; });

            if (options.selectionManager != nullptr)
                options.selectionManager->addToSelection (*pitch);
        }
    }

    return true;
}

//==============================================================================
//==============================================================================
Clipboard::TimeSigs::TimeSigs() {}
Clipboard::TimeSigs::~TimeSigs() {}

bool Clipboard::TimeSigs::pasteIntoEdit (const EditPastingOptions& options) const
{
    if (timeSigs.empty())
        return false;

    if (options.selectionManager != nullptr)
        options.selectionManager->deselectAll();

    auto startBeat = options.edit.tempoSequence.timeToBeats (snapTimeToNearestBeat (options.edit, options.startTime));
    auto firstTimeSigBeat = static_cast<double> (timeSigs.front().getProperty (IDs::startBeat));
    auto offset = startBeat - firstTimeSigBeat;
    auto um = &options.edit.getUndoManager();

    for (auto& state : timeSigs)
    {
        auto time = options.edit.tempoSequence.beatsToTime (offset + static_cast<double> (state.getProperty (IDs::startBeat)));

        if (auto timeSig = options.edit.tempoSequence.insertTimeSig (time))
        {
            jassert (timeSig->state.getNumChildren() == 0); // this would need handling
            copyValueTreeProperties (timeSig->state, state, um,
                                     [] (const juce::Identifier& name) { return name != IDs::startBeat; });

            if (options.selectionManager != nullptr)
                options.selectionManager->addToSelection (*timeSig);
        }
    }

    return true;
}

//==============================================================================
//==============================================================================
Clipboard::Plugins::Plugins (const Plugin::Array& items)
{
    for (auto& item : items)
    {
        item->edit.flushPluginStateIfNeeded (*item);
        plugins.push_back (item->state.createCopy());
        
        if (auto rackInstance = dynamic_cast<RackInstance*> (item))
        {
            if (auto type = rackInstance->type)
            {
                auto newEntry = std::make_pair (type->edit.getWeakRef(), type->state);
                
                if (std::find (rackTypes.begin(), rackTypes.end(), newEntry) == rackTypes.end())
                    rackTypes.push_back (newEntry);
            }
        }
    }
}

Clipboard::Plugins::~Plugins() {}

static bool pastePluginBasedOnSelection (Edit& edit, const Plugin::Ptr& newPlugin,
                                         SelectionManager* selectionManager)
{
    if (selectionManager == nullptr)
        return false;

    if (RackType::Ptr selectedRack = selectionManager->getFirstItemOfType<RackType>())
    {
        selectedRack->addPlugin (newPlugin, { 0.5f, 0.5f }, false);
        return true;
    }

    if (Plugin::Ptr selectedPlugin = selectionManager->getFirstItemOfType<Plugin>())
    {
        if (auto list = selectedPlugin->getOwnerList())
        {
            auto index = list->indexOf (selectedPlugin.get());

            if (index >= 0)
            {
                list->insertPlugin (newPlugin, index, selectionManager);
                return true;
            }
        }

        if (auto selectedRack = edit.getRackList().findRackContaining (*selectedPlugin))
        {
            selectedRack->addPlugin (newPlugin, { 0.5f, 0.5f }, false);
            return true;
        }
    }

    if (auto selectedClip = selectionManager->getFirstItemOfType<Clip>())
        if (selectedClip->addClipPlugin (newPlugin, *selectionManager))
            return true;

    return false;
}

static bool pastePluginIntoTrack (const Plugin::Ptr& newPlugin, EditInsertPoint& insertPoint, SelectionManager* sm)
{
    double startPos = 0.0;
    Track::Ptr track;
    insertPoint.chooseInsertPoint (track, startPos, false, sm,
                                   [] (auto& t) { return t.isAudioTrack() || t.isFolderTrack() || t.isMasterTrack(); });
    jassert (track != nullptr);

    if (track != nullptr && track->canContainPlugin (newPlugin.get()))
    {
        track->pluginList.insertPlugin (newPlugin, 0, sm);
        return true;
    }

    return false;
}

static EditItemID::IDMap pasteRackTypesInToEdit (Edit& edit, const std::vector<std::pair<Selectable::WeakRef, juce::ValueTree>>& editAndTypeStates)
{
    EditItemID::IDMap reassignedIDs;

    for (const auto& editAndTypeState : editAndTypeStates)
    {
        if (editAndTypeState.first == &edit)
            continue;
        
        auto typeState = editAndTypeState.second;
        auto reassignedRackType = typeState.createCopy();
        EditItemID::remapIDs (reassignedRackType, nullptr, edit, &reassignedIDs);
        edit.getRackList().addRackTypeFrom (reassignedRackType);
    }
    
    return reassignedIDs;
}

bool Clipboard::Plugins::pasteIntoEdit (const EditPastingOptions& options) const
{
    CRASH_TRACER
    bool anyPasted = false;
    
    auto rackIDMap = pasteRackTypesInToEdit (options.edit, rackTypes);

    auto pluginsToPaste = plugins;
    std::reverse (pluginsToPaste.begin(), pluginsToPaste.end()); // Reverse the array so they get pasted in the correct order

    for (auto& item : pluginsToPaste)
    {
        auto stateCopy = item.createCopy();
        EditItemID::remapIDs (stateCopy, nullptr, options.edit);
        
        // Remap RackTypes after the otehr IDs or it will get overwritten
        if (stateCopy[IDs::type].toString() == IDs::rack.toString())
        {
            auto oldRackID = EditItemID::fromProperty (stateCopy, IDs::rackType);
            auto remappedRackID = rackIDMap[oldRackID];
            
            if (remappedRackID.isValid())
                remappedRackID.setProperty (stateCopy, IDs::rackType, nullptr);
        }

        if (auto newPlugin = options.edit.getPluginCache().getOrCreatePluginFor (stateCopy))
        {
            if (pastePluginBasedOnSelection (options.edit, newPlugin, options.selectionManager)
                 || pastePluginIntoTrack (newPlugin, options.insertPoint, options.selectionManager))
            {
                anyPasted = true;

                // If we've pasted a plugin into a different track, see if it should still be under modifier control
                if (auto track = newPlugin->getOwnerTrack())
                {
                    for (auto param : newPlugin->getAutomatableParameters())
                    {
                        auto assignments = param->getAssignments();

                        for (int i = assignments.size(); --i >= 0;)
                        {
                            auto ass = assignments.getUnchecked (i);

                            if (auto mpa = dynamic_cast<MacroParameter::Assignment*> (ass.get()))
                            {
                                if (auto mp = getMacroParameterForID (options.edit, mpa->macroParamID))
                                    if (auto t = mp->getTrack())
                                        if (! (t == track || track->isAChildOf (*t)))
                                            param->removeModifier (*ass);
                            }
                            else
                            {
                                if (auto t = getTrackContainingModifier (options.edit,
                                                                         findModifierForID (options.edit, EditItemID::fromProperty (ass->state, IDs::source))))
                                    if (! (t == track || TrackList::isFixedTrack (t->state) || track->isAChildOf (*t)))
                                        param->removeModifier (*ass);
                            }
                        }
                    }
                }
            }
        }
    }

    return anyPasted;
}

//==============================================================================
//==============================================================================
Clipboard::Takes::Takes (const WaveCompManager& waveCompManager)
{
    items = waveCompManager.getActiveTakeTree().createCopy();
}

Clipboard::Takes::~Takes() {}

bool Clipboard::Takes::pasteIntoClip (WaveAudioClip& c) const
{
    if (items.isValid())
        return c.getCompManager().pasteComp (items).isValid();

    return false;
}

//==============================================================================
//==============================================================================
Clipboard::Modifiers::Modifiers() {}
Clipboard::Modifiers::~Modifiers() {}

bool Clipboard::Modifiers::pasteIntoEdit (const EditPastingOptions& options) const
{
    if (modifiers.empty())
        return false;

    if (options.selectionManager != nullptr)
    {
        if (auto firstSelectedMod = options.selectionManager->getFirstItemOfType<Modifier>())
        {
            if (auto t = getTrackContainingModifier (options.edit, firstSelectedMod))
            {
                auto modList = getModifiersOfType<Modifier> (t->getModifierList());

                for (int i = modList.size(); --i >= 0;)
                {
                    if (modList.getObjectPointer (i) == firstSelectedMod)
                    {
                        for (auto m : modifiers)
                        {
                            EditItemID::remapIDs (m, nullptr, options.edit);
                            t->getModifierList().insertModifier (m, i + 1, options.selectionManager);
                        }

                        return true;
                    }
                }
            }
        }
    }

    if (options.startTrack != nullptr && ! options.startTrack->isMarkerTrack())
    {
        for (auto m : modifiers)
        {
            EditItemID::remapIDs (m, nullptr, options.edit);
            options.startTrack->getModifierList().insertModifier (m, -1, options.selectionManager);
        }

        return true;
    }

    return false;
}

}
