local jobfile = assert(os.getenv('SLICELAB_REAPER_JOB'), 'missing job file')
local job = assert(loadfile(jobfile))()

local function write(path, value)
    local file = assert(io.open(path, 'wb'))
    file:write(value)
    file:close()
end

local function read(path)
    local file = assert(io.open(path, 'rb'))
    local value = file:read('*a')
    file:close()
    return value
end

local function named(track, fx, key)
    local ok, value = reaper.TrackFX_GetNamedConfigParm(track, fx, key)
    return ok and value or 'unavailable'
end

local function check_identity()
    assert(reaper.GetResourcePath() == job.resource, 'unexpected REAPER resource directory')
    local _, project = reaper.EnumProjects(-1, '')
    assert(project == job.project, 'unexpected REAPER project')
    if job.mode ~= 'cold' and job.mode ~= 'gui' and job.mode ~= 'live' then
        assert(reaper.CountTracks(0) == 0, 'expected an empty project')
    end
end

local function inventory(track, fx, parameters_path, metadata_path)
    local lines = {'index\tidentifier\tname\tnormalized'}
    for index = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
        local _, id = reaper.TrackFX_GetParamIdent(track, fx, index)
        local _, name = reaper.TrackFX_GetParamName(track, fx, index)
        local value = reaper.TrackFX_GetParamNormalized(track, fx, index)
        lines[#lines + 1] = string.format('%d\t%s\t%s\t%.12f', index, id, name, value)
    end
    write(parameters_path or job.parameters, table.concat(lines, '\n') .. '\n')
    local metadata = {'reaper_version\t' .. reaper.GetAppVersion(),
        'os\t' .. reaper.GetOS(),
        'resource\t' .. reaper.GetResourcePath(),
        'plugin_name\t' .. select(2, reaper.TrackFX_GetFXName(track, fx)),
        'plugin_identifier\t' .. named(track, fx, 'fx_ident'),
        'plugin_type\t' .. named(track, fx, 'fx_type'),
        'is_instrument\t' .. named(track, fx, 'is_instrument'),
        'pdc_frames\t' .. named(track, fx, 'pdc'),
        'channel_config\t' .. named(track, fx, 'channel_config')}
    local _, inputs, outputs = reaper.TrackFX_GetIOSize(track, fx)
    metadata[#metadata + 1] = 'input_pins\t' .. tostring(inputs)
    metadata[#metadata + 1] = 'output_pins\t' .. tostring(outputs)
    for _, key in ipairs({'MODE', 'SRATE', 'BSIZE', 'BPS'}) do
        local ok, value = reaper.GetAudioDeviceInfo(key)
        metadata[#metadata + 1] = 'audio_' .. key:lower() .. '\t' .. (ok and value or 'unavailable')
    end
    write(metadata_path or job.metadata, table.concat(metadata, '\n') .. '\n')
end

local function create_track()
    reaper.InsertTrackAtIndex(0, false)
    local track = assert(reaper.GetTrack(0, 0), 'could not create track')
    if job.channels > 2 then
        reaper.SetMediaTrackInfo_Value(track, 'I_NCHAN', job.channels)
        reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(0), 'I_NCHAN', job.channels)
        reaper.SetMediaTrackInfo_Value(track, 'C_MAINSEND_NCH', 0)
    end
    local fx = reaper.TrackFX_AddByName(track, 'VST3i: SliceLab', false, -1)
    assert(fx >= 0, 'SliceLab VST3 instrument was not found')
    local _, name = reaper.TrackFX_GetFXName(track, fx)
    assert(name:find('SliceLab', 1, true), 'loaded the wrong plugin')
    assert(reaper.TrackFX_GetInstrument(track) == fx, 'FX is not recognized as an instrument')
    assert(not reaper.TrackFX_GetOpen(track, fx), 'plugin editor unexpectedly open')
    if job.channels > 2 then
        assert(reaper.TrackFX_SetNamedConfigParm(track, fx, 'channel_config', tostring(job.channels)),
            'could not request multichannel VST3 configuration')
        for pin = 0, job.channels - 1 do
            local low, high = 0, 0
            if pin < 32 then low = 1 << pin else high = 1 << (pin - 32) end
            assert(reaper.TrackFX_SetPinMappings(track, fx, 1, pin, low, high), 'could not map output pin')
        end
    end
    return track, fx
end

local function set_render(name)
    for key, value in pairs({RENDER_SETTINGS=0, RENDER_BOUNDSFLAG=0,
            RENDER_CHANNELS=job.channels, RENDER_SRATE=48000, RENDER_STARTPOS=0,
            RENDER_ENDPOS=job.length, RENDER_TAILFLAG=0, RENDER_ADDTOPROJ=0,
            RENDER_DITHER=16, RENDER_NORMALIZE=0, PROJECT_SRATE=48000,
            PROJECT_SRATE_USE=1}) do
        reaper.GetSetProjectInfo(0, key, value, true)
    end
    assert(reaper.GetSetProjectInfo_String(0, 'RENDER_FILE', job.render_dir, true))
    assert(reaper.GetSetProjectInfo_String(0, 'RENDER_PATTERN', name, true))
    assert(reaper.GetSetProjectInfo_String(0, 'RENDER_FORMAT', job.render_format, true))
    assert(reaper.GetSetProjectInfo_String(0, 'RENDER_FORMAT2', '', true))
end

local function add_midi(track)
    local item = assert(reaper.CreateNewMIDIItemInProj(track, 0, job.length, false))
    reaper.SetMediaItemInfo_Value(item, 'B_LOOPSRC', 0)
    if job.midi_ppq > 0 then
        local ok, chunk = reaper.GetItemStateChunk(item, '', false)
        assert(ok, 'could not read MIDI item')
        local count
        chunk, count = chunk:gsub('(HASDATA 1 )%d+( QN)', function(prefix, suffix)
            return prefix .. tostring(job.midi_ppq) .. suffix
        end)
        assert(count == 1 and reaper.SetItemStateChunk(item, chunk, false), 'could not set MIDI resolution')
    end
    local take = assert(reaper.GetActiveTake(item))
    for _, note in ipairs(job.notes) do
        local first = reaper.MIDI_GetPPQPosFromProjTime(take, note.start)
        local last = reaper.MIDI_GetPPQPosFromProjTime(take, note.finish)
        assert(reaper.MIDI_InsertNote(take, false, false, first, last, 0, note.note, note.velocity, true))
    end
    reaper.MIDI_Sort(take)
end

local function add_gain_automation(track, fx, parameter)
    local index = nil
    for i = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
        local _, name = reaper.TrackFX_GetParamName(track, fx, i)
        if name == parameter then index = i break end
    end
    assert(index, 'automation parameter is missing: ' .. parameter)
    local envelope = assert(reaper.GetFXEnvelope(track, fx, index, true))
    assert(reaper.InsertEnvelopePoint(envelope, 0, 60 / 72, 1, 0, false, true))
    assert(reaper.InsertEnvelopePoint(envelope, 0.5, 0, 1, 0, false, true))
    reaper.Envelope_SortPoints(envelope)
    reaper.SetTrackAutomationMode(track, 1)
end

local function finish(ok, err)
    local temporary = job.result .. '.tmp'
    write(temporary, ok and 'OK\n' or ('ERROR\n' .. tostring(err) .. '\n'))
    assert(os.rename(temporary, job.result))
end

local function live_capture(track, fx)
    local ok_rate, rate = reaper.GetAudioDeviceInfo('SRATE')
    local ok_block, block = reaper.GetAudioDeviceInfo('BSIZE')
    local ok_mode, mode = reaper.GetAudioDeviceInfo('MODE')
    assert(ok_rate and tonumber(rate) == 48000 and ok_block and tonumber(block) == job.quantum and
        ok_mode and mode == 'JACK', 'unexpected JACK sample rate or buffer size')
    reaper.SetMediaTrackInfo_Value(track, 'I_PERFFLAGS', job.anticipative and 0 or 2)
    local midi = assert(reaper.GetTrackMediaItem(track, 0), 'live project has no MIDI item')
    reaper.SetMediaItemInfo_Value(midi, 'D_LENGTH', 3.0)
    if job.instrumented then
        local capture = reaper.TrackFX_AddByName(track, 'JS: SliceLabCapture', false, -1)
        assert(capture >= 0 and capture > fx, 'could not insert the transparent capture effect')
        reaper.gmem_attach(job.gmem)
        reaper.gmem_write(0, 0)
        reaper.gmem_write(1, 0)
        reaper.gmem_write(4, 0)
    end
    if job.live_source == 'virtual' then
        reaper.SetMediaItemInfo_Value(midi, 'B_MUTE', 1)
        reaper.SetOnlyTrackSelected(track)
        reaper.SetMediaTrackInfo_Value(track, 'I_RECINPUT', 4096 + (63 << 5))
        reaper.SetMediaTrackInfo_Value(track, 'I_RECARM', 1)
        reaper.SetMediaTrackInfo_Value(track, 'I_RECMON', 1)
    elseif job.live_source == 'graph' then
        reaper.SetMediaItemInfo_Value(midi, 'B_MUTE', 1)
        reaper.TrackFX_SetEnabled(track, fx, false)
        assert(not reaper.TrackFX_GetEnabled(track, fx), 'could not bypass SliceLab for graph control')
        local item = assert(reaper.AddMediaItemToTrack(track))
        local take = assert(reaper.AddTakeToMediaItem(item))
        local source = assert(reaper.PCM_Source_CreateFromFile(job.reference_audio))
        reaper.SetMediaItemTake_Source(take, source)
        assert(reaper.GetMediaItemTake_Source(take) == source, 'could not set graph control source')
        reaper.SetMediaItemInfo_Value(item, 'D_POSITION', 0.02)
        reaper.SetMediaItemInfo_Value(item, 'D_LENGTH', 1.0)
        reaper.SetMediaItemInfo_Value(item, 'B_LOOPSRC', 0)
        reaper.SetMediaItemInfo_Value(item, 'D_FADEINLEN', 0)
        reaper.SetMediaItemInfo_Value(item, 'D_FADEOUTLEN', 0)
    end
    inventory(track, fx)
    reaper.Main_SaveProject(0, false)
    write(job.ready, 'READY\n')
    local started, sent_on, sent_off, stopping = nil, false, false, false
    local note_on, note_off, stopped = 0, 0, 0
    local chain_pdc, plugin_pdc = 'unavailable', 'unavailable'

    local function save_capture()
        local frames, blocks = 0, 0
        if job.instrumented then
            frames = math.floor(reaper.gmem_read(0))
            blocks = math.floor(reaper.gmem_read(1))
            assert(frames > 0 and frames <= 96000 and blocks > 0 and blocks <= 4096, 'capture has no valid frames or blocks')
            local pcm = assert(io.open(job.capture, 'wb'))
            for i = 0, frames - 1 do
                pcm:write(string.pack('<ff', reaper.gmem_read(32768 + i * 2), reaper.gmem_read(32769 + i * 2)))
            end
            pcm:close()
            local lines = {'wall_seconds\tproject_seconds\tframes\tframe_offset'}
            for i = 0, blocks - 1 do
                local at = 100 + i * 4
                lines[#lines + 1] = string.format('%.9f\t%.9f\t%d\t%d', reaper.gmem_read(at),
                    reaper.gmem_read(at + 1), reaper.gmem_read(at + 2), reaper.gmem_read(at + 3))
            end
            write(job.cadence, table.concat(lines, '\n') .. '\n')
        end
        local first_audio = job.instrumented and reaper.gmem_read(4) or 0
        write(job.live_info, string.format('frames\t%d\nblocks\t%d\nstarted\t%.9f\nstopped\t%.9f\nnote_on\t%.9f\nnote_off\t%.9f\nfirst_audio_wall\t%.9f\nchain_pdc_frames\t%s\nplugin_pdc_during_playback\t%s\n',
            frames, blocks, started, stopped, note_on, note_off, first_audio, chain_pdc, plugin_pdc))
    end

    local tick
    tick = function()
        local ok, err = xpcall(function()
            if not started then
                if reaper.file_exists(job.go) then
                    reaper.SetEditCurPos(0, false, false)
                    started = reaper.time_precise()
                    reaper.OnPlayButton()
                end
            elseif stopping then
                save_capture()
                finish(true)
                return
            else
                local elapsed = reaper.time_precise() - started
                if job.live_source == 'virtual' and elapsed >= 0.30 and not sent_on then
                    note_on = reaper.time_precise()
                    reaper.StuffMIDIMessage(0, 0x90, 36, 127)
                    sent_on = true
                end
                if job.live_source == 'virtual' and elapsed >= 0.50 and not sent_off then
                    note_off = reaper.time_precise()
                    reaper.StuffMIDIMessage(0, 0x80, 36, 0)
                    sent_off = true
                end
                if elapsed >= 2.70 then
                    chain_pdc = named(track, fx, 'chain_pdc_actual')
                    plugin_pdc = named(track, fx, 'pdc')
                    stopped = reaper.time_precise()
                    reaper.OnStopButton()
                    stopping = true
                end
            end
            reaper.defer(tick)
        end, debug.traceback)
        if not ok then finish(false, err) end
    end
    reaper.defer(tick)
end

local function watch_gui(track, fx)
    local tick
    tick = function()
        if reaper.file_exists(job.gui_request) then
            local token, operation = read(job.gui_request):match('^([%w_-]+)%s*(%w*)')
            os.remove(job.gui_request)
            if token then
                local prefix = job.gui_dir .. '/' .. token
                local ok, err = xpcall(function()
                    check_identity()
                    assert(reaper.GetTrack(0, 0) == track and reaper.TrackFX_GetInstrument(track) == fx,
                        'inspection project changed')
                    assert(operation == '' or operation == 'snapshot' or operation == 'save', 'unknown inspection command')
                    if operation == 'save' then reaper.Main_SaveProject(0, false) end
                    local valid, chunk = reaper.TrackFX_GetNamedConfigParm(track, fx, 'vst_chunk')
                    assert(valid and #chunk > 0, 'could not read inspection state')
                    write(prefix .. '.b64', chunk)
                    inventory(track, fx, prefix .. '.parameters.tsv', prefix .. '.metadata.tsv')
                end, debug.traceback)
                write(prefix .. '.result', ok and 'OK\n' or ('ERROR\n' .. tostring(err) .. '\n'))
            end
        end
        reaper.defer(tick)
    end
    reaper.defer(tick)
end

local function run()
    check_identity()
    if job.mode == 'identity' then
        local metadata = {'reaper_version\t' .. reaper.GetAppVersion(), 'os\t' .. reaper.GetOS(),
            'resource\t' .. reaper.GetResourcePath()}
        for _, key in ipairs({'MODE', 'SRATE', 'BSIZE', 'BPS'}) do
            local ok, value = reaper.GetAudioDeviceInfo(key)
            metadata[#metadata + 1] = 'audio_' .. key:lower() .. '\t' .. (ok and value or 'unavailable')
        end
        write(job.metadata, table.concat(metadata, '\n') .. '\n')
        return
    end
    local track, fx
    if job.mode == 'cold' or job.mode == 'gui' or job.mode == 'live' then
        assert(reaper.CountTracks(0) == 1, 'cold project has unexpected track count')
        track = assert(reaper.GetTrack(0, 0))
        fx = reaper.TrackFX_GetInstrument(track)
        assert(fx >= 0, 'cold project lost its instrument')
        local _, name = reaper.TrackFX_GetFXName(track, fx)
        assert(name:find('SliceLab', 1, true), 'cold project loaded the wrong plugin')
        if job.mode ~= 'gui' then
            assert(not reaper.TrackFX_GetOpen(track, fx), 'project unexpectedly opened the editor')
        end
    else
        track, fx = create_track()
    end
    if job.mode == 'live' then
        live_capture(track, fx)
        return true
    elseif job.mode == 'gui' then
        if job.pipewire then
            local ok_rate, rate = reaper.GetAudioDeviceInfo('SRATE')
            local ok_block, block = reaper.GetAudioDeviceInfo('BSIZE')
            local ok_mode, mode = reaper.GetAudioDeviceInfo('MODE')
            assert(ok_rate and tonumber(rate) == 48000 and ok_block and tonumber(block) == job.quantum and
                ok_mode and mode == 'JACK', 'GUI session did not negotiate the requested JACK device')
        end
        inventory(track, fx)
        reaper.TrackFX_Show(track, fx, 3)
        assert(reaper.TrackFX_GetOpen(track, fx), 'could not open SliceLab editor')
        finish(true)
        watch_gui(track, fx)
        return true
    elseif job.mode == 'probe' then
        local ok, chunk = reaper.TrackFX_GetNamedConfigParm(track, fx, 'vst_chunk')
        assert(ok and #chunk > 0, 'could not read VST3 state')
        write(job.chunk, chunk)
        inventory(track, fx)
    elseif job.mode == 'case' then
        assert(reaper.TrackFX_SetNamedConfigParm(track, fx, 'vst_chunk', read(job.chunk)), 'could not set VST3 state')
        local ok, actual = reaper.TrackFX_GetNamedConfigParm(track, fx, 'vst_chunk')
        assert(ok and #actual > 0, 'could not read restored VST3 state')
        write(job.actual_chunk, actual)
        reaper.SetCurrentBPM(0, job.bpm, false)
        if #job.tempo_markers > 0 then
            for _, marker in ipairs(job.tempo_markers) do
                assert(reaper.SetTempoTimeSigMarker(0, -1, marker.time, -1, -1, marker.bpm, 0, 0, false),
                    'could not set a host tempo marker')
            end
            local marker_count = reaper.CountTempoTimeSigMarkers(0)
            assert(marker_count == #job.tempo_markers, 'unexpected number of host tempo markers')
            local lines = {'time_seconds\tbpm'}
            for index, marker in ipairs(job.tempo_markers) do
                local valid, actual_time, _, _, actual_bpm = reaper.GetTempoTimeSigMarker(0, index - 1)
                assert(valid and math.abs(actual_time - marker.time) < 0.000001 and
                    math.abs(actual_bpm - marker.bpm) < 0.000001,
                    string.format('host tempo marker differs: expected %.9f/%.9f, got %.9f/%.9f',
                                  marker.time, marker.bpm, actual_time, actual_bpm))
                lines[#lines + 1] = string.format('%.9f\t%.9f', actual_time, actual_bpm)
            end
            write(job.tempo_report, table.concat(lines, '\n') .. '\n')
        end
        add_midi(track)
        if job.automation_parameter ~= '' then add_gain_automation(track, fx, job.automation_parameter) end
        set_render(job.name)
        inventory(track, fx)
        reaper.Main_SaveProject(0, false)
        reaper.Main_OnCommand(42230, 0)
    elseif job.mode == 'cold' then
        set_render(job.name)
        local function render_cold()
            if job.inspect_tempo_map then
                local lines = {'seconds\tbpm'}
                for _, seconds in ipairs({0, 0.5, 1.5, 3, 6}) do
                    lines[#lines + 1] = string.format('%.3f\t%.9f', seconds, reaper.TimeMap2_GetDividedBpmAtTime(0, seconds))
                end
                write(job.tempo_report, table.concat(lines, '\n') .. '\n')
            end
            reaper.Main_OnCommand(42230, 0)
            inventory(track, fx)
        end
        if not job.cold_immediate then
            local started = reaper.time_precise()
            local observations = {'phase\ttick\telapsed_seconds\tquery_seconds\texpected_bpm\tactual_bpm'}
            local function check_tempo_map(phase, count)
                local ready = true
                for _, marker in ipairs(job.tempo_markers) do
                    local at = marker.time + 0.001
                    local actual = reaper.TimeMap2_GetDividedBpmAtTime(0, at)
                    observations[#observations + 1] = string.format('%s\t%d\t%.9f\t%.9f\t%.9f\t%.9f',
                        phase, count, reaper.time_precise() - started, at, marker.bpm, actual)
                    if math.abs(actual - marker.bpm) > 0.000001 then ready = false end
                end
                return ready
            end
            check_tempo_map('synchronous', 0)
            local count = 0
            local tick
            tick = function()
                count = count + 1
                local ready = check_tempo_map('deferred', count)
                local elapsed = reaper.time_precise() - started
                if elapsed > 5 then
                    write(job.cold_ready, table.concat(observations, '\n') .. '\n')
                    finish(false, 'REAPER tempo map did not become ready within five seconds')
                elseif not ready or elapsed < job.cold_delay then
                    reaper.defer(tick)
                else
                    observations[#observations + 1] = string.format('render\t%d\t%.9f\t\t\t', count, elapsed)
                    write(job.cold_ready, table.concat(observations, '\n') .. '\n')
                    local ok, err = xpcall(function()
                        check_identity()
                        render_cold()
                        reaper.Main_SaveProject(0, false)
                    end, debug.traceback)
                    finish(ok, err)
                end
            end
            reaper.defer(tick)
            return true
        end
        render_cold()
    else
        error('unknown job mode')
    end
    reaper.Main_SaveProject(0, false)
end

local ok, result = xpcall(run, debug.traceback)
if not ok then finish(false, result) elseif not result then finish(true) end
