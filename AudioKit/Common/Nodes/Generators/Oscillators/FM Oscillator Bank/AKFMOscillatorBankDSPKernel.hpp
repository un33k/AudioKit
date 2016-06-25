//
//  AKFMOscillatorBankDSPKernel.hpp
//  AudioKit
//
//  Created by Aurelius Prochazka, revision history on Github.
//  Copyright (c) 2016 Aurelius Prochazka. All rights reserved.
//

#ifndef AKFMOscillatorBankDSPKernel_hpp
#define AKFMOscillatorBankDSPKernel_hpp

#import "DSPKernel.hpp"
#import "ParameterRamper.hpp"
#import <vector>

#import <AudioKit/AudioKit-Swift.h>

extern "C" {
#include "soundpipe.h"
}

enum {
    carrierMultiplierAddress = 0,
    modulatingMultiplierAddress = 1,
    modulationIndexAddress = 2,
    attackDurationAddress = 3,
    releaseDurationAddress = 4,
    detuningOffsetAddress = 5,
    detuningMultiplierAddress = 6
};

static inline double pow2(double x) {
    return x * x;
}

static inline double noteToHz(int noteNumber)
{
    return 440. * exp2((noteNumber - 69)/12.);
}

class AKFMOscillatorBankDSPKernel : public DSPKernel {
public:
    // MARK: Types
    struct NoteState {
        NoteState* next;
        NoteState* prev;
        AKFMOscillatorBankDSPKernel* kernel;
        
        enum { stageOff, stageAttack, stageSustain, stageRelease };
        double envLevel = 0.;
        double envSlope = 0.;
  
        int stage = stageOff;
        int envRampSamples = 0;
        
        sp_fosc *fosc;
        
        void init() {
            sp_fosc_create(&fosc);
            sp_fosc_init(kernel->sp, fosc, kernel->ftbl);
            fosc->freq = 0;
            fosc->amp = 0;
        }

        
        void clear() {
            stage = stageOff;
            envLevel = 0.;
        }
        
        // linked list management
        void remove() {
            if (prev) prev->next = next;
            else kernel->playingNotes = next;
            
            if (next) next->prev = prev;
            
            prev = next = nullptr;
            
            --kernel->playingNotesCount;

            sp_fosc_destroy(&fosc);
        }
        
        void add() {
            init();
            prev = nullptr;
            next = kernel->playingNotes;
            if (next) next->prev = this;
            kernel->playingNotes = this;
            ++kernel->playingNotesCount;
        }
        
        void noteOn(int noteNumber, int velocity)
        {
            if (velocity == 0) {
                if (stage == stageAttack || stage == stageSustain) {
                    stage = stageRelease;
                    envRampSamples = kernel->releaseSamples;
                    envSlope = -envLevel / envRampSamples;
                }
            } else {
                if (stage == stageOff) { add(); }
                fosc->freq = (float)noteToHz(noteNumber);
                fosc->amp = (float)pow2(velocity / 127.);
                stage = stageAttack;
                envRampSamples = kernel->attackSamples;
                envSlope = (1.0 - envLevel) / envRampSamples;
            }
        }
        
        
        void run(int n, float* outL, float* outR)
        {
            int framesRemaining = n;
            
            float originalFrequency = fosc->freq;
            fosc->freq *= kernel->detuningMultiplier;
            fosc->freq += kernel->detuningOffset;
            fosc->freq = clamp(fosc->freq, 0.0f, 22050.0f);
            fosc->car = kernel->carrierMultiplier;
            fosc->mod = kernel->modulatingMultiplier;
            fosc->indx = kernel->modulationIndex;
            
            while (framesRemaining) {
                switch (stage) {
                    case stageOff :
                        NSLog(@"stageOff on playingNotes list!");
                        return;
                    case stageAttack : {
                        int framesThisTime = std::min(framesRemaining, envRampSamples);
                        for (int i = 0; i < framesThisTime; ++i) {
                            float x = 0;
                            sp_fosc_compute(kernel->sp, fosc, nil, &x);
                            *outL++ += envLevel * x;
                            *outR++ += envLevel * x;
                            
                            envLevel += envSlope;
                        }

                        framesRemaining -= framesThisTime;
                        envRampSamples -= framesThisTime;
                        if (envRampSamples == 0) {
                            stage = stageSustain;
                        }
                        fosc->freq = originalFrequency;
                        break;
                    }
                    case stageSustain : {
                        for (int i = 0; i < framesRemaining; ++i) {
                            float x = 0;
                            sp_fosc_compute(kernel->sp, fosc, nil, &x);
                            *outL++ += envLevel * x;
                            *outR++ += envLevel * x;
                        }
                        fosc->freq = originalFrequency;
                        return;
                    }
                    case stageRelease : {
                        int framesThisTime = std::min(framesRemaining, envRampSamples);
                        for (int i = 0; i < framesThisTime; ++i) {
                            float x = 0;
                            sp_fosc_compute(kernel->sp, fosc, nil, &x);
                            *outL++ += envLevel * x;
                            *outR++ += envLevel * x;
                            envLevel += envSlope;
                        }
                        envRampSamples -= framesThisTime;
                        fosc->freq = originalFrequency;
                        if (envRampSamples == 0) {
                            clear();
                            remove();
                        }
                        return;
                    }
                    default:
                        NSLog(@"bad stage on playingNotes list!");
                        return;
                }
            }
            
        }
        
    };

    // MARK: Member Functions

    AKFMOscillatorBankDSPKernel() {
        noteStates.resize(128);
        for (NoteState& state : noteStates) {
            state.kernel = this;
        }
    }

    void init(int channelCount, double inSampleRate) {
        channels = channelCount;

        sampleRate = float(inSampleRate);

        sp_create(&sp);
        sp->sr = sampleRate;
        sp->nchan = channels;
        
        attackDurationRamper.init();
        releaseDurationRamper.init();
        detuningOffsetRamper.init();
        detuningMultiplierRamper.init();
    }

    void setupWaveform(uint32_t size) {
        ftbl_size = size;
        sp_ftbl_create(sp, &ftbl, ftbl_size);
    }

    void setWaveformValue(uint32_t index, float value) {
        ftbl->tbl[index] = value;
    }

    void startNote(int note, int velocity) {
        noteStates[note].noteOn(note, velocity);
    }

    void stopNote(int note) {
        noteStates[note].noteOn(note, 0);
    }

    void destroy() {
        sp_destroy(&sp);
    }

    void reset() {
        for (NoteState& state : noteStates) {
            state.clear();
        }
        playingNotes = nullptr;
        playingNotesCount = 0;
        resetted = true;
        
        attackDurationRamper.reset();
        releaseDurationRamper.reset();
        detuningOffsetRamper.reset();
        detuningMultiplierRamper.reset();
    }
    
    void setCarrierMultiplier(float value) {
        carrierMultiplier = clamp(value, 0.0f, 1000.0f);
        carrierMultiplierRamper.setImmediate(carrierMultiplier);
    }
    
    void setModulatingMultiplier(float value) {
        modulatingMultiplier = clamp(value, 0.0f, 1000.0f);
        modulatingMultiplierRamper.setImmediate(modulatingMultiplier);
    }
    
    void setModulationIndex(float value) {
        modulationIndex = clamp(value, 0.0f, 1000.0f);
        modulationIndexRamper.setImmediate(modulationIndex);
    }

    void setAttackDuration(float value) {
        attackDuration = clamp(value, (float)0, (float)10);
        attackDurationRamper.setImmediate(attackDuration);
        attackSamples = sampleRate * attackDuration;
    }

    void setReleaseDuration(float value) {
        releaseDuration = clamp(value, (float)0, (float)100);
        releaseDurationRamper.setImmediate(releaseDuration);
        releaseSamples = sampleRate * releaseDuration;
    }
    
    void setDetuningOffset(float value) {
        detuningOffset = clamp(value, (float)-1000, (float)1000);
        detuningOffsetRamper.setImmediate(detuningOffset);
    }

    void setDetuningMultiplier(float value) {
        detuningMultiplier = clamp(value, (float)0.9, (float)1.11);
        detuningMultiplierRamper.setImmediate(detuningMultiplier);
    }


    void setParameter(AUParameterAddress address, AUValue value) {
        switch (address) {
                
            case carrierMultiplierAddress:
                carrierMultiplierRamper.setUIValue(clamp(value, 0.0f, 1000.0f));
                break;
                
            case modulatingMultiplierAddress:
                modulatingMultiplierRamper.setUIValue(clamp(value, 0.0f, 1000.0f));
                break;
                
            case modulationIndexAddress:
                modulationIndexRamper.setUIValue(clamp(value, 0.0f, 1000.0f));
                break;

            case attackDurationAddress:
                attackDuration = clamp(value, 0.001f, 10.f);
                attackSamples = sampleRate * attackDuration;
                break;
                
            case releaseDurationAddress:
                releaseDuration = clamp(value, 0.001f, 100.f);
                releaseSamples = sampleRate * releaseDuration;
                break;
                
            case detuningOffsetAddress:
                detuningOffsetRamper.setUIValue(clamp(value, (float)-1000, (float)1000));
                break;

            case detuningMultiplierAddress:
                detuningMultiplierRamper.setUIValue(clamp(value, (float)0.9, (float)1.11));
                break;

        }
    }

    AUValue getParameter(AUParameterAddress address) {
        switch (address) {

            case carrierMultiplierAddress:
                return carrierMultiplierRamper.getUIValue();
                
            case modulatingMultiplierAddress:
                return modulatingMultiplierRamper.getUIValue();
                
            case modulationIndexAddress:
                return modulationIndexRamper.getUIValue();
                
            case attackDurationAddress:
                return attackDurationRamper.getUIValue();

            case releaseDurationAddress:
                return releaseDurationRamper.getUIValue();
            
            case detuningOffsetAddress:
                return detuningOffsetRamper.getUIValue();

            case detuningMultiplierAddress:
                return detuningMultiplierRamper.getUIValue();

            default: return 0.0f;
        }
    }

    void startRamp(AUParameterAddress address, AUValue value, AUAudioFrameCount duration) override {
        switch (address) {
                
            case carrierMultiplierAddress:
                carrierMultiplierRamper.startRamp(clamp(value, 0.0f, 1000.0f), duration);
                break;
                
            case modulatingMultiplierAddress:
                modulatingMultiplierRamper.startRamp(clamp(value, 0.0f, 1000.0f), duration);
                break;
                
            case modulationIndexAddress:
                modulationIndexRamper.startRamp(clamp(value, 0.0f, 1000.0f), duration);
                break;
            case attackDurationAddress:
                attackDurationRamper.startRamp(clamp(value, (float)-1000, (float)1000), duration);
                break;
                
            case releaseDurationAddress:
                releaseDurationRamper.startRamp(clamp(value, (float)-1000, (float)1000), duration);
                break;

            case detuningOffsetAddress:
                detuningOffsetRamper.startRamp(clamp(value, (float)-1000, (float)1000), duration);
                break;

            case detuningMultiplierAddress:
                detuningMultiplierRamper.startRamp(clamp(value, (float)0.9, (float)1.11), duration);
                break;

        }
    }

    void setBuffer(AudioBufferList *outBufferList) {
        outBufferListPtr = outBufferList;
    }
    
    virtual void handleMIDIEvent(AUMIDIEvent const& midiEvent) override {
        if (midiEvent.length != 3) return;
        uint8_t status = midiEvent.data[0] & 0xF0;
        //uint8_t channel = midiEvent.data[0] & 0x0F; // works in omni mode.
        switch (status) {
            case 0x80 : { // note off
                uint8_t note = midiEvent.data[1];
                if (note > 127) break;
                noteStates[note].noteOn(note, 0);
                break;
            }
            case 0x90 : { // note on
                uint8_t note = midiEvent.data[1];
                uint8_t veloc = midiEvent.data[2];
                if (note > 127 || veloc > 127) break;
                noteStates[note].noteOn(note, veloc);
                break;
            }
            case 0xB0 : { // control
                uint8_t num = midiEvent.data[1];
                if (num == 123) { // all notes off
                    NoteState* noteState = playingNotes;
                    while (noteState) {
                        noteState->clear();
                        noteState = noteState->next;
                    }
                    playingNotes = nullptr;
                    playingNotesCount = 0;
                }
                break;
            }
        }
    }

    void process(AUAudioFrameCount frameCount, AUAudioFrameCount bufferOffset) override {

        float* outL = (float*)outBufferListPtr->mBuffers[0].mData + bufferOffset;
        float* outR = (float*)outBufferListPtr->mBuffers[1].mData + bufferOffset;

        carrierMultiplier = double(carrierMultiplierRamper.getAndStep());
        modulatingMultiplier = double(modulatingMultiplierRamper.getAndStep());
        modulationIndex = double(modulationIndexRamper.getAndStep());
        attackDuration = double(attackDurationRamper.getAndStep());
        attackSamples = sampleRate * attackDuration;
        releaseDuration = double(releaseDurationRamper.getAndStep());
        releaseSamples = sampleRate * releaseDuration;
        detuningOffset = double(detuningOffsetRamper.getAndStep());
        detuningMultiplier = double(detuningMultiplierRamper.getAndStep());
        
        for (AUAudioFrameCount i = 0; i < frameCount; ++i) {
            outL[i] = 0.0f;
            outR[i] = 0.0f;
        }
        
        NoteState* noteState = playingNotes;
        while (noteState) {
            noteState->run(frameCount, outL, outR);
            noteState = noteState->next;
        }

        
        for (AUAudioFrameCount i = 0; i < frameCount; ++i) {
            outL[i] *= .5f;
            outR[i] *= .5f;
        }
    }

    // MARK: Member Variables

private:
    std::vector<NoteState> noteStates;

    int channels = AKSettings.numberOfChannels;
    float sampleRate = AKSettings.sampleRate;
    double frequencyScale = 2. * M_PI / sampleRate;

    AudioBufferList *outBufferListPtr = nullptr;

    sp_data *sp;
    sp_ftbl *ftbl;
    UInt32 ftbl_size = 4096;
    
    float carrierMultiplier = 1.0;
    float modulatingMultiplier = 1;
    float modulationIndex = 1;

    float attackDuration = 0;
    float releaseDuration = 0;

    float detuningOffset = 0;
    float detuningMultiplier = 1;

public:
    NoteState* playingNotes = nullptr;
    int playingNotesCount = 0;
    bool resetted = false;

    int attackSamples   = sampleRate * attackDuration;
    int releaseSamples  = sampleRate * releaseDuration;

    
    ParameterRamper carrierMultiplierRamper = 1.0;
    ParameterRamper modulatingMultiplierRamper = 1;
    ParameterRamper modulationIndexRamper = 1;

    ParameterRamper attackDurationRamper = 0;
    ParameterRamper releaseDurationRamper = 0;

    ParameterRamper detuningOffsetRamper = 0;
    ParameterRamper detuningMultiplierRamper = 1;
};

#endif /* AKFMOscillatorBankDSPKernel_hpp */