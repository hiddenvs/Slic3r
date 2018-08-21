#include <limits>

#include "libslic3r.h"
#include "Slicing.hpp"
#include "SlicingAdaptive.hpp"
#include "PrintConfig.hpp"
#include "Model.hpp"

// #define SLIC3R_DEBUG

// Make assert active if SLIC3R_DEBUG
#ifdef SLIC3R_DEBUG
    #undef NDEBUG
    #define DEBUG
    #define _DEBUG
    #include "SVG.hpp"
    #undef assert 
    #include <cassert>
#endif

namespace Slic3r
{

static const coordf_t MIN_LAYER_HEIGHT = 0.01;
static const coordf_t MIN_LAYER_HEIGHT_DEFAULT = 0.07;

// Minimum layer height for the variable layer height algorithm.
inline coordf_t min_layer_height_from_nozzle(const PrintConfig &print_config, int idx_nozzle)
{
    coordf_t min_layer_height = print_config.min_layer_height.get_at(idx_nozzle - 1);
    return (min_layer_height == 0.) ? MIN_LAYER_HEIGHT_DEFAULT : std::max(MIN_LAYER_HEIGHT, min_layer_height);
}

// Maximum layer height for the variable layer height algorithm, 3/4 of a nozzle dimaeter by default,
// it should not be smaller than the minimum layer height.
inline coordf_t max_layer_height_from_nozzle(const PrintConfig &print_config, int idx_nozzle)
{
    coordf_t min_layer_height = min_layer_height_from_nozzle(print_config, idx_nozzle);
    coordf_t max_layer_height = print_config.max_layer_height.get_at(idx_nozzle - 1);
    coordf_t nozzle_dmr       = print_config.nozzle_diameter.get_at(idx_nozzle - 1);
    return std::max(min_layer_height, (max_layer_height == 0.) ? (0.75 * nozzle_dmr) : max_layer_height);
}

SlicingParameters SlicingParameters::create_from_config(
	const PrintConfig 		&print_config, 
	const PrintObjectConfig &object_config,
	coordf_t				 object_height,
	const std::vector<unsigned int> &object_extruders)
{
    coordf_t first_layer_height                      = (object_config.first_layer_height.value <= 0) ? 
        object_config.layer_height.value : 
        object_config.first_layer_height.get_abs_value(object_config.layer_height.value);
    // If object_config.support_material_extruder == 0 resp. object_config.support_material_interface_extruder == 0,
    // print_config.nozzle_diameter.get_at(size_t(-1)) returns the 0th nozzle diameter,
    // which is consistent with the requirement that if support_material_extruder == 0 resp. support_material_interface_extruder == 0,
    // support will not trigger tool change, but it will use the current nozzle instead.
    // In that case all the nozzles have to be of the same diameter.
    coordf_t support_material_extruder_dmr           = print_config.nozzle_diameter.get_at(object_config.support_material_extruder.value - 1);
    coordf_t support_material_interface_extruder_dmr = print_config.nozzle_diameter.get_at(object_config.support_material_interface_extruder.value - 1);
    bool     soluble_interface                       = object_config.support_material_contact_distance.value == 0.;

    SlicingParameters params;
    params.layer_height = object_config.layer_height.value;
    params.first_print_layer_height = first_layer_height;
    params.first_object_layer_height = first_layer_height;
    params.object_print_z_min = 0.;
    params.object_print_z_max = object_height;
    params.base_raft_layers = object_config.raft_layers.value;
    params.soluble_interface = soluble_interface;

    // Miniumum/maximum of the minimum layer height over all extruders.
    params.min_layer_height = MIN_LAYER_HEIGHT;
    params.max_layer_height = std::numeric_limits<double>::max();
    if (object_config.support_material.value || params.base_raft_layers > 0) {
        // Has some form of support. Add the support layers to the minimum / maximum layer height limits.
        params.min_layer_height = std::max(
            min_layer_height_from_nozzle(print_config, object_config.support_material_extruder), 
            min_layer_height_from_nozzle(print_config, object_config.support_material_interface_extruder));
        params.max_layer_height = std::min(
            max_layer_height_from_nozzle(print_config, object_config.support_material_extruder), 
            max_layer_height_from_nozzle(print_config, object_config.support_material_interface_extruder));
        params.max_suport_layer_height = params.max_layer_height;
    }
    if (object_extruders.empty()) {
        params.min_layer_height = std::max(params.min_layer_height, min_layer_height_from_nozzle(print_config, 0));
        params.max_layer_height = std::min(params.max_layer_height, max_layer_height_from_nozzle(print_config, 0));
    } else {
        for (unsigned int extruder_id : object_extruders) {
            params.min_layer_height = std::max(params.min_layer_height, min_layer_height_from_nozzle(print_config, extruder_id));
            params.max_layer_height = std::min(params.max_layer_height, max_layer_height_from_nozzle(print_config, extruder_id));
        }
    }
    params.min_layer_height = std::min(params.min_layer_height, params.layer_height);
    params.max_layer_height = std::max(params.max_layer_height, params.layer_height);

    if (! soluble_interface) {
        params.gap_raft_object    = object_config.support_material_contact_distance.value;
        params.gap_object_support = object_config.support_material_contact_distance.value;
        params.gap_support_object = object_config.support_material_contact_distance.value;
    }

    if (params.base_raft_layers > 0) {
		params.interface_raft_layers = (params.base_raft_layers + 1) / 2;
        params.base_raft_layers -= params.interface_raft_layers;
        // Use as large as possible layer height for the intermediate raft layers.
        params.base_raft_layer_height       = std::max(params.layer_height, 0.75 * support_material_extruder_dmr);
        params.interface_raft_layer_height  = std::max(params.layer_height, 0.75 * support_material_interface_extruder_dmr);
        params.contact_raft_layer_height_bridging = false;
        params.first_object_layer_bridging  = false;
        #if 1
        params.contact_raft_layer_height    = std::max(params.layer_height, 0.75 * support_material_interface_extruder_dmr);
        if (! soluble_interface) {
            // Compute the average of all nozzles used for printing the object over a raft.
            //FIXME It is expected, that the 1st layer of the object is printed with a bridging flow over a full raft. Shall it not be vice versa?
            coordf_t average_object_extruder_dmr = 0.;
            if (! object_extruders.empty()) {
                for (unsigned int extruder_id : object_extruders)
                    average_object_extruder_dmr += print_config.nozzle_diameter.get_at(extruder_id);
                average_object_extruder_dmr /= coordf_t(object_extruders.size());
            }
            params.first_object_layer_height   = average_object_extruder_dmr;
            params.first_object_layer_bridging = true;
        }
        #else
        params.contact_raft_layer_height    = soluble_interface ? support_material_interface_extruder_dmr : 0.75 * support_material_interface_extruder_dmr;
        params.contact_raft_layer_height_bridging = ! soluble_interface;
        ...
        #endif
    }

    if (params.has_raft()) {
        // Raise first object layer Z by the thickness of the raft itself plus the extra distance required by the support material logic.
        //FIXME The last raft layer is the contact layer, which shall be printed with a bridging flow for ease of separation. Currently it is not the case.
		if (params.raft_layers() == 1) {
            // There is only the contact layer.
			params.contact_raft_layer_height = first_layer_height;
            params.raft_contact_top_z = first_layer_height;
		} else {
            assert(params.base_raft_layers > 0);
            assert(params.interface_raft_layers > 0);
            // Number of the base raft layers is decreased by the first layer.
            params.raft_base_top_z       = first_layer_height + coordf_t(params.base_raft_layers - 1) * params.base_raft_layer_height;
            // Number of the interface raft layers is decreased by the contact layer.
            params.raft_interface_top_z  = params.raft_base_top_z + coordf_t(params.interface_raft_layers - 1) * params.interface_raft_layer_height;
			params.raft_contact_top_z    = params.raft_interface_top_z + params.contact_raft_layer_height;
		}
        coordf_t print_z = params.raft_contact_top_z + params.gap_raft_object;
        params.object_print_z_min  = print_z;
        params.object_print_z_max += print_z;
    }

    return params;
}

// Convert layer_height_ranges to layer_height_profile. Both are referenced to z=0, meaning the raft layers are not accounted for
// in the height profile and the printed object may be lifted by the raft thickness at the time of the G-code generation.
std::vector<coordf_t> layer_height_profile_from_ranges(
	const SlicingParameters 	&slicing_params,
	const t_layer_height_ranges &layer_height_ranges)
{
    // 1) If there are any height ranges, trim one by the other to make them non-overlapping. Insert the 1st layer if fixed.
    std::vector<std::pair<t_layer_height_range,coordf_t>> ranges_non_overlapping;
    ranges_non_overlapping.reserve(layer_height_ranges.size() * 4);
    if (slicing_params.first_object_layer_height_fixed())
        ranges_non_overlapping.push_back(std::pair<t_layer_height_range,coordf_t>(
            t_layer_height_range(0., slicing_params.first_object_layer_height), 
            slicing_params.first_object_layer_height));
    // The height ranges are sorted lexicographically by low / high layer boundaries.
    for (t_layer_height_ranges::const_iterator it_range = layer_height_ranges.begin(); it_range != layer_height_ranges.end(); ++ it_range) {
        coordf_t lo = it_range->first.first;
        coordf_t hi = std::min(it_range->first.second, slicing_params.object_print_z_height());
        coordf_t height = it_range->second;
        if (! ranges_non_overlapping.empty())
            // Trim current low with the last high.
            lo = std::max(lo, ranges_non_overlapping.back().first.second);
        if (lo + EPSILON < hi)
            // Ignore too narrow ranges.
            ranges_non_overlapping.push_back(std::pair<t_layer_height_range,coordf_t>(t_layer_height_range(lo, hi), height));
    }

    // 2) Convert the trimmed ranges to a height profile, fill in the undefined intervals between z=0 and z=slicing_params.object_print_z_max()
    // with slicing_params.layer_height
    std::vector<coordf_t> layer_height_profile;
    for (std::vector<std::pair<t_layer_height_range,coordf_t>>::const_iterator it_range = ranges_non_overlapping.begin(); it_range != ranges_non_overlapping.end(); ++ it_range) {
        coordf_t lo = it_range->first.first;
        coordf_t hi = it_range->first.second;
        coordf_t height = it_range->second;
        coordf_t last_z      = layer_height_profile.empty() ? 0. : layer_height_profile[layer_height_profile.size() - 2];
        coordf_t last_height = layer_height_profile.empty() ? 0. : layer_height_profile[layer_height_profile.size() - 1];
        if (lo > last_z + EPSILON) {
            // Insert a step of normal layer height.
            layer_height_profile.push_back(last_z);
            layer_height_profile.push_back(slicing_params.layer_height);
            layer_height_profile.push_back(lo);
            layer_height_profile.push_back(slicing_params.layer_height);
        }
        // Insert a step of the overriden layer height.
        layer_height_profile.push_back(lo);
        layer_height_profile.push_back(height);
        layer_height_profile.push_back(hi);
        layer_height_profile.push_back(height);
    }

    coordf_t last_z      = layer_height_profile.empty() ? 0. : layer_height_profile[layer_height_profile.size() - 2];
    coordf_t last_height = layer_height_profile.empty() ? 0. : layer_height_profile[layer_height_profile.size() - 1];
    if (last_z < slicing_params.object_print_z_height()) {
        // Insert a step of normal layer height up to the object top.
        layer_height_profile.push_back(last_z);
        layer_height_profile.push_back(slicing_params.layer_height);
        layer_height_profile.push_back(slicing_params.object_print_z_height());
        layer_height_profile.push_back(slicing_params.layer_height);
    }

   	return layer_height_profile;
}

// Based on the work of @platsch
// Fill layer_height_profile by heights ensuring a prescribed maximum cusp height.
std::vector<coordf_t> layer_height_profile_adaptive(
    const SlicingParameters     &slicing_params,
    const t_layer_height_ranges &layer_height_ranges,
    const ModelVolumePtrs		&volumes)
{
    // 1) Initialize the SlicingAdaptive class with the object meshes.
    SlicingAdaptive as;
    as.set_slicing_parameters(slicing_params);
    for (ModelVolumePtrs::const_iterator it = volumes.begin(); it != volumes.end(); ++ it)
        if (! (*it)->modifier)
            as.add_mesh(&(*it)->mesh);
    as.prepare();

    // 2) Generate layers using the algorithm of @platsch 
    // loop until we have at least one layer and the max slice_z reaches the object height
    //FIXME make it configurable
    // Cusp value: A maximum allowed distance from a corner of a rectangular extrusion to a chrodal line, in mm.
    const coordf_t cusp_value = 0.2; // $self->config->get_value('cusp_value');

    std::vector<coordf_t> layer_height_profile;
    layer_height_profile.push_back(0.);
    layer_height_profile.push_back(slicing_params.first_object_layer_height);
    if (slicing_params.first_object_layer_height_fixed()) {
        layer_height_profile.push_back(slicing_params.first_object_layer_height);
        layer_height_profile.push_back(slicing_params.first_object_layer_height);
    }
    coordf_t slice_z = slicing_params.first_object_layer_height;
    coordf_t height  = slicing_params.first_object_layer_height;
    coordf_t cusp_height = 0.;
    int current_facet = 0;
    while ((slice_z - height) <= slicing_params.object_print_z_height()) {
        height = 999;
        // Slic3r::debugf "\n Slice layer: %d\n", $id;
        // determine next layer height
        coordf_t cusp_height = as.cusp_height(slice_z, cusp_value, current_facet);
        // check for horizontal features and object size
        /*
        if($self->config->get_value('match_horizontal_surfaces')) {
            my $horizontal_dist = $adaptive_slicing[$region_id]->horizontal_facet_distance(scale $slice_z+$cusp_height, $min_height);
            if(($horizontal_dist < $min_height) && ($horizontal_dist > 0)) {
                Slic3r::debugf "Horizontal feature ahead, distance: %f\n", $horizontal_dist;
                # can we shrink the current layer a bit?
                if($cusp_height-($min_height-$horizontal_dist) > $min_height) {
                    # yes we can
                    $cusp_height = $cusp_height-($min_height-$horizontal_dist);
                    Slic3r::debugf "Shrink layer height to %f\n", $cusp_height;
                }else{
                    # no, current layer would become too thin
                    $cusp_height = $cusp_height+$horizontal_dist;
                    Slic3r::debugf "Widen layer height to %f\n", $cusp_height;
                }
            }
        }
        */
        height = std::min(cusp_height, height);

        // apply z-gradation
        /*
        my $gradation = $self->config->get_value('adaptive_slicing_z_gradation');
        if($gradation > 0) {
            $height = $height - unscale((scale($height)) % (scale($gradation)));
        }
        */
    
        // look for an applicable custom range
        /*
        if (my $range = first { $_->[0] <= $slice_z && $_->[1] > $slice_z } @{$self->layer_height_ranges}) {
            $height = $range->[2];
    
            # if user set custom height to zero we should just skip the range and resume slicing over it
            if ($height == 0) {
                $slice_z += $range->[1] - $range->[0];
                next;
            }
        }
        */
        
        layer_height_profile.push_back(slice_z);
        layer_height_profile.push_back(height);
        slice_z += height;
        layer_height_profile.push_back(slice_z);
        layer_height_profile.push_back(height);
    }

    coordf_t last = std::max(slicing_params.first_object_layer_height, layer_height_profile[layer_height_profile.size() - 2]);
    layer_height_profile.push_back(last);
    layer_height_profile.push_back(slicing_params.first_object_layer_height);
    layer_height_profile.push_back(slicing_params.object_print_z_height());
    layer_height_profile.push_back(slicing_params.first_object_layer_height);

    return layer_height_profile;
}

void adjust_layer_height_profile(
    const SlicingParameters     &slicing_params,
    std::vector<coordf_t> 		&layer_height_profile,
    coordf_t 					 z,
    coordf_t 					 layer_thickness_delta,
    coordf_t 					 band_width,
    LayerHeightEditActionType    action)
{
     // Constrain the profile variability by the 1st layer height.
    std::pair<coordf_t, coordf_t> z_span_variable = 
        std::pair<coordf_t, coordf_t>(
            slicing_params.first_object_layer_height_fixed() ? slicing_params.first_object_layer_height : 0.,
            slicing_params.object_print_z_height());
    if (z < z_span_variable.first || z > z_span_variable.second)
        return;

	assert(layer_height_profile.size() >= 2);
    assert(std::abs(layer_height_profile[layer_height_profile.size() - 2] - slicing_params.object_print_z_height()) < EPSILON);

    // 1) Get the current layer thickness at z.
    coordf_t current_layer_height = slicing_params.layer_height;
    for (size_t i = 0; i < layer_height_profile.size(); i += 2) {
        if (i + 2 == layer_height_profile.size()) {
            current_layer_height = layer_height_profile[i + 1];
            break;
        } else if (layer_height_profile[i + 2] > z) {
            coordf_t z1 = layer_height_profile[i];
            coordf_t h1 = layer_height_profile[i + 1];
            coordf_t z2 = layer_height_profile[i + 2];
            coordf_t h2 = layer_height_profile[i + 3];
            current_layer_height = lerp(h1, h2, (z - z1) / (z2 - z1));
			break;
        }
    }

    // 2) Is it possible to apply the delta?
    switch (action) {
        case LAYER_HEIGHT_EDIT_ACTION_DECREASE:
            layer_thickness_delta = - layer_thickness_delta;
            // fallthrough
        case LAYER_HEIGHT_EDIT_ACTION_INCREASE:
            if (layer_thickness_delta > 0) {
                if (current_layer_height >= slicing_params.max_layer_height - EPSILON)
                    return;
                layer_thickness_delta = std::min(layer_thickness_delta, slicing_params.max_layer_height - current_layer_height);
            } else {
                if (current_layer_height <= slicing_params.min_layer_height + EPSILON)
                    return;
                layer_thickness_delta = std::max(layer_thickness_delta, slicing_params.min_layer_height - current_layer_height);
            }
            break;
        case LAYER_HEIGHT_EDIT_ACTION_REDUCE:
        case LAYER_HEIGHT_EDIT_ACTION_SMOOTH:
            layer_thickness_delta = std::abs(layer_thickness_delta);
            layer_thickness_delta = std::min(layer_thickness_delta, std::abs(slicing_params.layer_height - current_layer_height));
            if (layer_thickness_delta < EPSILON)
                return;
            break;
        default:
            assert(false);
            break;
    }

    // 3) Densify the profile inside z +- band_width/2, remove duplicate Zs from the height profile inside the band.
	coordf_t lo = std::max(z_span_variable.first,  z - 0.5 * band_width);
    // Do not limit the upper side of the band, so that the modifications to the top point of the profile will be allowed.
    coordf_t hi = z + 0.5 * band_width;
    coordf_t z_step = 0.1;
    size_t idx = 0;
    while (idx < layer_height_profile.size() && layer_height_profile[idx] < lo)
        idx += 2;
    idx -= 2;

    std::vector<double> profile_new;
    profile_new.reserve(layer_height_profile.size());
	assert(idx >= 0 && idx + 1 < layer_height_profile.size());
	profile_new.insert(profile_new.end(), layer_height_profile.begin(), layer_height_profile.begin() + idx + 2);
    coordf_t zz = lo;
    size_t i_resampled_start = profile_new.size();
    while (zz < hi) {
        size_t next = idx + 2;
        coordf_t z1 = layer_height_profile[idx];
        coordf_t h1 = layer_height_profile[idx + 1];
        coordf_t height = h1;
        if (next < layer_height_profile.size()) {
            coordf_t z2 = layer_height_profile[next];
            coordf_t h2 = layer_height_profile[next + 1];
            height = lerp(h1, h2, (zz - z1) / (z2 - z1));
        }
        // Adjust height by layer_thickness_delta.
        coordf_t weight = std::abs(zz - z) < 0.5 * band_width ? (0.5 + 0.5 * cos(2. * M_PI * (zz - z) / band_width)) : 0.;
        coordf_t height_new = height;
        switch (action) {
            case LAYER_HEIGHT_EDIT_ACTION_INCREASE:
            case LAYER_HEIGHT_EDIT_ACTION_DECREASE:
                height += weight * layer_thickness_delta;
                break;
            case LAYER_HEIGHT_EDIT_ACTION_REDUCE:
            {
                coordf_t delta = height - slicing_params.layer_height;
                coordf_t step  = weight * layer_thickness_delta;
                step = (std::abs(delta) > step) ?
                    (delta > 0) ? -step : step :
                    -delta;
                height += step;
                break;
            }
            case LAYER_HEIGHT_EDIT_ACTION_SMOOTH:
            {
                // Don't modify the profile during resampling process, do it at the next step.
                break;
            }
            default:
                assert(false);
                break;
        }
        height = clamp(slicing_params.min_layer_height, slicing_params.max_layer_height, height);
        if (zz == z_span_variable.second) {
            // This is the last point of the profile.
            if (profile_new[profile_new.size() - 2] + EPSILON > zz) {
                profile_new.pop_back();
                profile_new.pop_back();
            }
            profile_new.push_back(zz);
            profile_new.push_back(height);
			idx = layer_height_profile.size();
            break;
        }
        // Avoid entering a too short segment.
        if (profile_new[profile_new.size() - 2] + EPSILON < zz) {
            profile_new.push_back(zz);
            profile_new.push_back(height);
        }
        // Limit zz to the object height, so the next iteration the last profile point will be set.
		zz = std::min(zz + z_step, z_span_variable.second);
        idx = next;
        while (idx < layer_height_profile.size() && layer_height_profile[idx] < zz)
            idx += 2;
        idx -= 2;
    }

    idx += 2;
    assert(idx > 0);
    size_t i_resampled_end = profile_new.size();
	if (idx < layer_height_profile.size()) {
        assert(zz >= layer_height_profile[idx - 2]);
        assert(zz <= layer_height_profile[idx]);
		profile_new.insert(profile_new.end(), layer_height_profile.begin() + idx, layer_height_profile.end());
	}
	else if (profile_new[profile_new.size() - 2] + 0.5 * EPSILON < z_span_variable.second) { 
		profile_new.insert(profile_new.end(), layer_height_profile.end() - 2, layer_height_profile.end());
	}
    layer_height_profile = std::move(profile_new);

    if (action == LAYER_HEIGHT_EDIT_ACTION_SMOOTH) {
        if (i_resampled_start == 0)
            ++ i_resampled_start;
		if (i_resampled_end == layer_height_profile.size())
			i_resampled_end -= 2;
        size_t n_rounds = 6;
        for (size_t i_round = 0; i_round < n_rounds; ++ i_round) {
            profile_new = layer_height_profile;
            for (size_t i = i_resampled_start; i < i_resampled_end; i += 2) {
                coordf_t zz = profile_new[i];
                coordf_t t = std::abs(zz - z) < 0.5 * band_width ? (0.25 + 0.25 * cos(2. * M_PI * (zz - z) / band_width)) : 0.;
                assert(t >= 0. && t <= 0.5000001);
                if (i == 0)
                    layer_height_profile[i + 1] = (1. - t) * profile_new[i + 1] + t * profile_new[i + 3];
                else if (i + 1 == profile_new.size())
                    layer_height_profile[i + 1] = (1. - t) * profile_new[i + 1] + t * profile_new[i - 1];
                else
                    layer_height_profile[i + 1] = (1. - t) * profile_new[i + 1] + 0.5 * t * (profile_new[i - 1] + profile_new[i + 3]);
            }
        }
    }

	assert(layer_height_profile.size() > 2);
	assert(layer_height_profile.size() % 2 == 0);
	assert(layer_height_profile[0] == 0.);
    assert(std::abs(layer_height_profile[layer_height_profile.size() - 2] - slicing_params.object_print_z_height()) < EPSILON);
#ifdef _DEBUG
	for (size_t i = 2; i < layer_height_profile.size(); i += 2)
		assert(layer_height_profile[i - 2] <= layer_height_profile[i]);
	for (size_t i = 1; i < layer_height_profile.size(); i += 2) {
		assert(layer_height_profile[i] > slicing_params.min_layer_height - EPSILON);
		assert(layer_height_profile[i] < slicing_params.max_layer_height + EPSILON);
	}
#endif /* _DEBUG */
}

// Produce object layers as pairs of low / high layer boundaries, stored into a linear vector.
std::vector<coordf_t> generate_object_layers(
	const SlicingParameters 	&slicing_params,
	const std::vector<coordf_t> &layer_height_profile)
{
    assert(! layer_height_profile.empty());

    coordf_t print_z = 0;
    coordf_t height  = 0;

    std::vector<coordf_t> out;

    if (slicing_params.first_object_layer_height_fixed()) {
        out.push_back(0);
        print_z = slicing_params.first_object_layer_height;
        out.push_back(print_z);
    }

    size_t idx_layer_height_profile = 0;
    // loop until we have at least one layer and the max slice_z reaches the object height
    coordf_t slice_z = print_z + 0.5 * slicing_params.min_layer_height;
    while (slice_z < slicing_params.object_print_z_height()) {
        height = slicing_params.min_layer_height;
        if (idx_layer_height_profile < layer_height_profile.size()) {
            size_t next = idx_layer_height_profile + 2;
            for (;;) {
                if (next >= layer_height_profile.size() || slice_z < layer_height_profile[next])
                    break;
                idx_layer_height_profile = next;
                next += 2;
            }
            coordf_t z1 = layer_height_profile[idx_layer_height_profile];
            coordf_t h1 = layer_height_profile[idx_layer_height_profile + 1];
            height = h1;
            if (next < layer_height_profile.size()) {
                coordf_t z2 = layer_height_profile[next];
                coordf_t h2 = layer_height_profile[next + 1];
                height = lerp(h1, h2, (slice_z - z1) / (z2 - z1));
                assert(height >= slicing_params.min_layer_height - EPSILON && height <= slicing_params.max_layer_height + EPSILON);
            }
        }
        slice_z = print_z + 0.5 * height;
        if (slice_z >= slicing_params.object_print_z_height())
            break;
        assert(height > slicing_params.min_layer_height - EPSILON);
        assert(height < slicing_params.max_layer_height + EPSILON);
        out.push_back(print_z);
        print_z += height;
        slice_z = print_z + 0.5 * slicing_params.min_layer_height;
        out.push_back(print_z);
    }

    //FIXME Adjust the last layer to align with the top object layer exactly?
    return out;
}

int generate_layer_height_texture(
	const SlicingParameters 	&slicing_params,
	const std::vector<coordf_t> &layers,
	void *data, int rows, int cols, bool level_of_detail_2nd_level)
{
// https://github.com/aschn/gnuplot-colorbrewer
    std::vector<Point3> palette_raw;
    palette_raw.push_back(Point3(0x01A, 0x098, 0x050));
    palette_raw.push_back(Point3(0x066, 0x0BD, 0x063));
    palette_raw.push_back(Point3(0x0A6, 0x0D9, 0x06A));
    palette_raw.push_back(Point3(0x0D9, 0x0F1, 0x0EB));
    palette_raw.push_back(Point3(0x0FE, 0x0E6, 0x0EB));
    palette_raw.push_back(Point3(0x0FD, 0x0AE, 0x061));
    palette_raw.push_back(Point3(0x0F4, 0x06D, 0x043));
    palette_raw.push_back(Point3(0x0D7, 0x030, 0x027));

    // Clear the main texture and the 2nd LOD level.
//	memset(data, 0, rows * cols * (level_of_detail_2nd_level ? 5 : 4));
    // 2nd LOD level data start
    unsigned char *data1 = reinterpret_cast<unsigned char*>(data) + rows * cols * 4;
    int ncells  = std::min((cols-1) * rows, int(ceil(16. * (slicing_params.object_print_z_height() / slicing_params.min_layer_height))));
    int ncells1 = ncells / 2;
    int cols1   = cols / 2;
    coordf_t z_to_cell = coordf_t(ncells-1) / slicing_params.object_print_z_height();
    coordf_t cell_to_z = slicing_params.object_print_z_height() / coordf_t(ncells-1);
    coordf_t z_to_cell1 = coordf_t(ncells1-1) / slicing_params.object_print_z_height();
    // for color scaling
	coordf_t hscale = 2.f * std::max(slicing_params.max_layer_height - slicing_params.layer_height, slicing_params.layer_height - slicing_params.min_layer_height);
	if (hscale == 0)
		// All layers have the same height. Provide some height scale to avoid division by zero.
		hscale = slicing_params.layer_height;
    for (size_t idx_layer = 0; idx_layer < layers.size(); idx_layer += 2) {
        coordf_t lo  = layers[idx_layer];
		coordf_t hi  = layers[idx_layer + 1];
        coordf_t mid = 0.5f * (lo + hi);
		assert(mid <= slicing_params.object_print_z_height());
		coordf_t h = hi - lo;
		hi = std::min(hi, slicing_params.object_print_z_height());
        int cell_first = clamp(0, ncells-1, int(ceil(lo * z_to_cell)));
        int cell_last  = clamp(0, ncells-1, int(floor(hi * z_to_cell)));
        for (int cell = cell_first; cell <= cell_last; ++ cell) {
            coordf_t idxf = (0.5 * hscale + (h - slicing_params.layer_height)) * coordf_t(palette_raw.size()-1) / hscale;
            int idx1 = clamp(0, int(palette_raw.size() - 1), int(floor(idxf)));
            int idx2 = std::min(int(palette_raw.size() - 1), idx1 + 1);
			coordf_t t = idxf - coordf_t(idx1);
            const Point3 &color1 = palette_raw[idx1];
            const Point3 &color2 = palette_raw[idx2];
            coordf_t z = cell_to_z * coordf_t(cell);
			assert(z >= lo && z <= hi);
            // Intensity profile to visualize the layers.
            coordf_t intensity = cos(M_PI * 0.7 * (mid - z) / h);
            // Color mapping from layer height to RGB.
            Vec3d color(
                intensity * lerp(coordf_t(color1(0)), coordf_t(color2(0)), t), 
                intensity * lerp(coordf_t(color1(1)), coordf_t(color2(1)), t),
                intensity * lerp(coordf_t(color1(2)), coordf_t(color2(2)), t));
            int row = cell / (cols - 1);
            int col = cell - row * (cols - 1);
			assert(row >= 0 && row < rows);
			assert(col >= 0 && col < cols);
            unsigned char *ptr = (unsigned char*)data + (row * cols + col) * 4;
            ptr[0] = (unsigned char)clamp<int>(0, 255, int(floor(color(0) + 0.5)));
            ptr[1] = (unsigned char)clamp<int>(0, 255, int(floor(color(1) + 0.5)));
            ptr[2] = (unsigned char)clamp<int>(0, 255, int(floor(color(2) + 0.5)));
            ptr[3] = 255;
            if (col == 0 && row > 0) {
                // Duplicate the first value in a row as a last value of the preceding row.
                ptr[-4] = ptr[0];
                ptr[-3] = ptr[1];
                ptr[-2] = ptr[2];
                ptr[-1] = ptr[3];
            }
        }
        if (level_of_detail_2nd_level) {
            cell_first = clamp(0, ncells1-1, int(ceil(lo * z_to_cell1))); 
            cell_last  = clamp(0, ncells1-1, int(floor(hi * z_to_cell1)));
            for (int cell = cell_first; cell <= cell_last; ++ cell) {
                coordf_t idxf = (0.5 * hscale + (h - slicing_params.layer_height)) * coordf_t(palette_raw.size()-1) / hscale;
                int idx1 = clamp(0, int(palette_raw.size() - 1), int(floor(idxf)));
                int idx2 = std::min(int(palette_raw.size() - 1), idx1 + 1);
    			coordf_t t = idxf - coordf_t(idx1);
                const Point3 &color1 = palette_raw[idx1];
                const Point3 &color2 = palette_raw[idx2];
                // Color mapping from layer height to RGB.
                Vec3d color(
                    lerp(coordf_t(color1(0)), coordf_t(color2(0)), t), 
                    lerp(coordf_t(color1(1)), coordf_t(color2(1)), t),
                    lerp(coordf_t(color1(2)), coordf_t(color2(2)), t));
                int row = cell / (cols1 - 1);
                int col = cell - row * (cols1 - 1);
    			assert(row >= 0 && row < rows/2);
    			assert(col >= 0 && col < cols/2);
                unsigned char *ptr = data1 + (row * cols1 + col) * 4;
                ptr[0] = (unsigned char)clamp<int>(0, 255, int(floor(color(0) + 0.5)));
                ptr[1] = (unsigned char)clamp<int>(0, 255, int(floor(color(1) + 0.5)));
                ptr[2] = (unsigned char)clamp<int>(0, 255, int(floor(color(2) + 0.5)));
                ptr[3] = 255;
                if (col == 0 && row > 0) {
                    // Duplicate the first value in a row as a last value of the preceding row.
                    ptr[-4] = ptr[0];
                    ptr[-3] = ptr[1];
                    ptr[-2] = ptr[2];
                    ptr[-1] = ptr[3];
                }
            }
        }
    }

    // Returns number of cells of the 0th LOD level.
    return ncells;
}

}; // namespace Slic3r
