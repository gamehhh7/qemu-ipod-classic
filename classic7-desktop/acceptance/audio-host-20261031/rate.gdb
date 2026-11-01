set pagination off
set confirm off
attach 1824
p i2s_live.rate
p i2s_live.rd
p i2s_live.wr
p i2s_live.primed
p *i2s_live.voice
p *i2s_live.voice->hw
detach
quit
