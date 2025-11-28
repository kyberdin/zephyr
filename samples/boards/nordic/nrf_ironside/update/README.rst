.. zephyr:code-sample:: nrf_ironside_update
   :name: Nordic IronSide SE firmware update

   Update the Nordic IronSide SE firmware.

Overview
********

The Nordic IronSide SE Update sample updates the IronSide SE firmware on a SoC that already has IronSide SE installed.
It can update both the main image and the recovery image of IronSide SE using the IronSide SE firmware release ZIP file.

See the official `Ironside SE documentation`_ for details about the service and update process.

.. _Ironside SE documentation: https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/app_dev/device_guides/nrf54h/ug_nrf54h20_ironside_update.html

Building and running the application for nrf54h20dk/nrf54h20/cpuapp
*******************************************************************

.. note::
   You can use this application only when there is already a version of IronSide SE installed on the device.

1. Unzip the IronSide SE release ZIP to get the update hex file:

   .. code-block:: console

      unzip nrf54h20_soc_binaries_v.*.zip

#. Program the appropriate update hex file from the release ZIP using one (not both) of the following commands:

   a) To update IronSide SE firmware:

      .. code-block:: console

         nrfutil device program --traits jlink --firmware update/ironside_se_update.hex

   b) To update IronSide SE recovery firmware:

      .. code-block:: console

         nrfutil device program --traits jlink --firmware update/ironside_se_recovery_update.hex

#. Build and program the application:

   .. zephyr-app-commands::
      :zephyr-app: samples/boards/nordic/nrf_ironside/update
      :board: nrf54h20dk/nrf54h20/cpuapp
      :goals: flash

#. Trigger a reset:

   .. code-block:: console

      nrfutil device reset --reset-kind RESET_PIN --traits jlink

#. Check the boot report to verify that the new versions have been installed:

   .. code-block:: console

      nrfutil device x-ironside-boot-report-read --traits jlink
