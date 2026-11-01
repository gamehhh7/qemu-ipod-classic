set pagination off
set confirm off
attach 1824
p audiolive
p i2s_live.registered
p i2s_live.voice
p i2s_live.primed
p i2s_live.rate
p i2s_live.rd
p i2s_live.wr
p i2s_live.card.state->drv->name
p i2s_live.voice->active
p i2s_live.voice->hw->enabled
detach
quit
