"""Fundamental-presence observation for the deliberately voiced SID output."""
import numpy as np

def observe(samples, expected_hz, sample_rate=22050):
    samples=np.asarray(samples)
    power=abs(np.fft.rfft(samples*np.hanning(len(samples))))
    freq=np.fft.rfftfreq(len(samples),1/sample_rate)
    dominant=1+power[1:].argmax()
    band=np.flatnonzero(abs(freq-expected_hz)<max(3,expected_hz*.02))
    root=band[power[band].argmax()]
    ratio=float(power[root]/max(float(power[dominant]),1e-30))
    return {'expected_hz':expected_hz,'dominant_hz':float(freq[dominant]),'root_hz':float(freq[root]),'root_relative_peak':ratio,
            'root_present':bool(ratio>.3 and abs(freq[root]-expected_hz)<max(2,expected_hz*.02))}

if __name__=='__main__':
    t=np.arange(44100)/22050
    fundamental=np.sin(2*np.pi*130.813*t)
    voiced=.7*fundamental+np.sin(2*np.pi*261.626*t)+.4*np.sin(2*np.pi*196.2195*t)
    assert observe(voiced,130.813)['root_present']
    assert not observe(np.sin(2*np.pi*261.626*t),130.813)['root_present']
    assert not observe(np.zeros_like(t),130.813)['root_present']
    print('SID voiced-root positive, wrong-octave and silence negative controls PASS')
