#!/usr/bin/env ruby
# One-shot migration script for ai_plans/03-ios-metal-render-pipeline.md §4/§9:
#   1. Adds EffectsRenderController.swift to the Runner target's Sources.
#   2. Adds a Run Script build phase that copies the per-slice lumacore.metallib
#      into the app bundle's resources (device vs simulator chosen dynamically
#      via $PLATFORM_NAME at Xcode build time, not baked in at project-edit time).
#   3. Sets STRIP_STYLE = non-global on the Runner target so dart:ffi-only
#      symbols (visibility(default) + used, but never called from Obj-C/Swift)
#      survive the final link.
#
# Run once: `ruby tools/ios/add_metal_pipeline_to_xcodeproj.rb`. Re-running is
# safe — every step checks for an existing entry first.
require 'xcodeproj'

project_path = File.expand_path('../../ios/Runner.xcodeproj', __dir__)
project = Xcodeproj::Project.open(project_path)

runner_target = project.targets.find { |t| t.name == 'Runner' }
raise 'Runner target not found' unless runner_target

runner_group = project.main_group.find_subpath('Runner', false)
raise 'Runner group not found' unless runner_group

# --- 1. EffectsRenderController.swift -------------------------------------
swift_file_name = 'EffectsRenderController.swift'
existing_ref = runner_group.files.find { |f| f.path == swift_file_name }
if existing_ref
  puts "#{swift_file_name} already in project — skipping file add."
else
  file_ref = runner_group.new_reference(swift_file_name)
  file_ref.include_in_index = '1'
  file_ref.last_known_file_type = 'sourcecode.swift'
  runner_target.source_build_phase.add_file_reference(file_ref)
  puts "Added #{swift_file_name} to Runner/Sources."
end

# --- 2. Run Script build phase: copy lumacore.metallib ---------------------
metallib_phase_name = 'Copy LumaCore Metal Library'
existing_phase = runner_target.build_phases.find do |p|
  p.respond_to?(:name) && p.name == metallib_phase_name
end
if existing_phase
  puts "'#{metallib_phase_name}' phase already present — skipping."
else
  phase = runner_target.new_shell_script_build_phase(metallib_phase_name)
  phase.shell_script = <<~SH
    set -e
    SRC_DIR="${SRCROOT}/../native/build-ios-device"
    if [ "${PLATFORM_NAME}" = "iphonesimulator" ]; then
      SRC_DIR="${SRCROOT}/../native/build-ios-sim"
    fi
    if [ ! -f "${SRC_DIR}/lumacore.metallib" ]; then
      echo "error: ${SRC_DIR}/lumacore.metallib not found — run tools/ios/build_xcframework.sh first." >&2
      exit 1
    fi
    mkdir -p "${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}"
    cp "${SRC_DIR}/lumacore.metallib" "${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}/lumacore.metallib"
  SH
  phase.input_paths = [
    '$(SRCROOT)/../native/build-ios-device/lumacore.metallib',
    '$(SRCROOT)/../native/build-ios-sim/lumacore.metallib',
  ]
  phase.output_paths = ['$(BUILT_PRODUCTS_DIR)/$(UNLOCALIZED_RESOURCES_FOLDER_PATH)/lumacore.metallib']
  phase.show_env_vars_in_log = '1'

  # Place it right after the "Resources" phase — it's conceptually a resource
  # copy step, and must run before "Embed Frameworks"/"Thin Binary" finalize
  # the bundle.
  resources_index = runner_target.build_phases.index(runner_target.resources_build_phase)
  runner_target.build_phases.delete(phase)
  runner_target.build_phases.insert(resources_index + 1, phase)
  puts "Added '#{metallib_phase_name}' Run Script phase."
end

# --- 3. STRIP_STYLE = non-global on the Runner target -----------------------
runner_target.build_configuration_list.build_configurations.each do |config|
  if config.build_settings['STRIP_STYLE'] == 'non-global'
    puts "STRIP_STYLE already non-global for #{config.name} — skipping."
  else
    config.build_settings['STRIP_STYLE'] = 'non-global'
    puts "Set STRIP_STYLE = non-global for #{config.name}."
  end
end

project.save
puts 'Saved project.pbxproj.'
