/* POST-LAYOUT REBALANCING: Kjører ETTER alle andre systemer for å tvinge lik fordeling */

#include "nixtile.h"

/* Enkel funksjon som tvinger lik høyde-fordeling ETTER at layout er ferdig */
void post_layout_force_equal_heights(Monitor *m) {
    if (!m) {
        wlr_log(WLR_ERROR, "[nixtile] POST-LAYOUT: NULL monitor");
        return;
    }
    
    wlr_log(WLR_ERROR, "[nixtile] *** POST-LAYOUT REBALANCE *** Tvinger lik fordeling ETTER layout");
    
    /* Tell tiles i hver kolonne */
    int tiles_per_column[MAX_COLUMNS] = {0};
    Client *c;
    
    wl_list_for_each(c, &clients, link) {
        if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen) {
            if (c->column_group >= 0 && c->column_group < MAX_COLUMNS) {
                tiles_per_column[c->column_group]++;
            }
        }
    }
    
    /* Tving lik fordeling i hver kolonne */
    for (int col = 0; col < MAX_COLUMNS; col++) {
        if (tiles_per_column[col] > 1) {
            wlr_log(WLR_ERROR, "[nixtile] POST-LAYOUT: Kolonne %d har %d tiles - tvinger %.1f%% hver", 
                    col, tiles_per_column[col], 100.0f / tiles_per_column[col]);
            
            wl_list_for_each(c, &clients, link) {
                if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen && 
                    c->column_group == col) {
                    
                    float old_factor = c->height_factor;
                    c->height_factor = 1.0f;
                    
                    wlr_log(WLR_ERROR, "[nixtile] POST-LAYOUT: Tile %p kolonne %d: %.3f -> 1.000", 
                            (void*)c, col, old_factor);
                }
            }
        }
    }
    
    wlr_log(WLR_ERROR, "[nixtile] *** POST-LAYOUT COMPLETE *** Alle kolonner rebalansert");
}

/* Kjør post-layout rebalancing med delay for å sikre at alt annet er ferdig */
int post_layout_rebalance_callback(void *data) {
    Monitor *m = (Monitor*)data;
    
    wlr_log(WLR_ERROR, "[nixtile] POST-LAYOUT CALLBACK: Kjører delayed rebalancing");
    post_layout_force_equal_heights(m);
    
    /* Tving en siste layout-oppdatering */
    if (m && m->wlr_output && m->wlr_output->enabled) {
        arrange(m);
    }
    
    return 0; /* Ikke gjenta */
}

/* Planlegg post-layout rebalancing med kort delay */
void schedule_post_layout_rebalance(Monitor *m) {
    if (!m) return;
    
    wlr_log(WLR_ERROR, "[nixtile] SCHEDULING: Post-layout rebalancing om 50ms");
    
    /* Bruk Wayland event loop timer for å kjøre rebalancing etter at alt annet er ferdig */
    struct wl_event_source *timer = wl_event_loop_add_timer(
        wl_display_get_event_loop(dpy), 
        post_layout_rebalance_callback, 
        m
    );
    
    if (timer) {
        wl_event_source_timer_update(timer, 50); /* 50ms delay */
    }
}
