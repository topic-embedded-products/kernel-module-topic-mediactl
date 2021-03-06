// SPDX-License-Identifier: GPL-2.0
/*
 * "Dummy" media driver to provide a /dev/media0 without a frame grabber
 * (C) Copyright 2020 Topic Embedded Products B.V. (http://www.topic.nl).
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/nvmem-consumer.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Topic Embedded Products <www.topic.nl>");

/**
 * struct xvip_composite_device - Xilinx Video IP device structure
 * @v4l2_dev: V4L2 device
 * @media_dev: media device
 * @dev: (OF) device
 * @notifier: V4L2 asynchronous subdevs notifier
 * @entities: entities in the graph as a list of xvip_graph_entity
 * @num_subdevs: number of subdevs in the pipeline
 */
struct xvip_composite_device {
	struct v4l2_device v4l2_dev;
	struct media_device media_dev;
	struct device *dev;

	struct v4l2_async_notifier notifier;
	struct list_head entities;
	unsigned int num_subdevs;

	bool is_streaming;
};


/**
 * struct xvip_graph_entity - Entity in the video graph
 * @asd: subdev asynchronous registration information
 * @entity: media entity, from the corresponding V4L2 subdev
 * @subdev: V4L2 subdev
 * @streaming: status of the V4L2 subdev if streaming or not
 */
struct xvip_graph_entity {
	struct v4l2_async_subdev asd;
	struct media_entity *entity;

	
	struct v4l2_subdev *subdev;
	bool streaming;
};

static struct xvip_composite_device *g_xdev;
static int indication = 0;


static inline struct xvip_graph_entity *
to_xvip_entity(struct v4l2_async_subdev *asd)
{
	return container_of(asd, struct xvip_graph_entity, asd);
}


/* -----------------------------------------------------------------------------
 * Graph Management
 */
 
static struct xvip_graph_entity *
xvip_graph_find_entity(struct xvip_composite_device *xdev,
		       const struct fwnode_handle *fwnode)
{
	struct xvip_graph_entity *entity;
	struct v4l2_async_subdev *asd;

	list_for_each_entry(asd, &xdev->notifier.asd_list, asd_list) {
		entity = to_xvip_entity(asd);
		if (entity->asd.match.fwnode == fwnode)
			return entity;
	}

	return NULL;
}

static struct xvip_graph_entity *
xvip_graph_find_entity_from_media(struct xvip_composite_device *xdev,
				  struct media_entity *entity)
{
	struct xvip_graph_entity *xvip_entity;
	struct v4l2_async_subdev *asd;

	list_for_each_entry(asd, &xdev->notifier.asd_list, asd_list) {
		xvip_entity = to_xvip_entity(asd);
		if (xvip_entity->entity == entity)
			return xvip_entity;
	}

	return NULL;
}


static int xvip_graph_build_one(struct xvip_composite_device *xdev,
				struct xvip_graph_entity *entity)
{
	u32 link_flags = MEDIA_LNK_FL_ENABLED;
	struct media_entity *local = entity->entity;
	struct media_entity *remote;
	struct media_pad *local_pad;
	struct media_pad *remote_pad;
	struct xvip_graph_entity *ent;
	struct v4l2_fwnode_link link;
	struct fwnode_handle *ep = NULL;
	int ret = 0;

	//dev_dbg(xdev->dev, "creating links for entity (%s)\n", local->name);
	if(strcmp(local->name,"IMX274") == 0) {
		if(indication == 0)
			local->name = "IMX274_0";
		else
			local->name = "IMX274_1";
		indication++;
		//dev_dbg(xdev->dev, "device is now (%s)\n", local->name);
	}

	while (1) {
		/* Get the next endpoint and parse its link. */
		ep = fwnode_graph_get_next_endpoint(entity->asd.match.fwnode,
						    ep);
		if (ep == NULL)
			break;

		//dev_dbg(xdev->dev, "processing endpoint %pOF\n", ep);

		ret = v4l2_fwnode_parse_link(ep, &link);
		if (ret < 0) {
			dev_err(xdev->dev, "failed to parse link for %pOF\n",
				ep);
			continue;
		}

		/* Skip sink ports, they will be processed from the other end of
		 * the link.
		 */
		if (link.local_port >= local->num_pads) {
			dev_err(xdev->dev, "invalid port number %u for %pOF\n",
				link.local_port,
				link.local_node);
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		local_pad = &local->pads[link.local_port];

		if (local_pad->flags & MEDIA_PAD_FL_SINK) {
			//dev_dbg(xdev->dev, "skipping sink port %pOF:%u\n",
			//	to_of_node(link.local_node),
			//	link.local_port);
			v4l2_fwnode_put_link(&link);
			continue;
		}

		/* Skip DMA engines, they will be processed separately. */
		if (link.remote_node == of_fwnode_handle(xdev->dev->of_node)) {
			//dev_dbg(xdev->dev, "skipping DMA port %pOF:%u\n",
			//	to_of_node(link.local_node),
			//	link.local_port);
			v4l2_fwnode_put_link(&link);
			continue;
		}

		/* Find the remote entity. */
		ent = xvip_graph_find_entity(xdev, link.remote_node);
		if (ent == NULL) {
			dev_err(xdev->dev, "no entity found for %p\n",
				link.remote_node);
			v4l2_fwnode_put_link(&link);
			ret = -ENODEV;
			break;
		}

		remote = ent->entity;

		if (link.remote_port >= remote->num_pads) {
			dev_err(xdev->dev, "invalid port number %u on %pOF\n",
				link.remote_port, link.remote_node);
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		remote_pad = &remote->pads[link.remote_port];

		v4l2_fwnode_put_link(&link);

		/* Create the media link. */
		//dev_dbg(xdev->dev, "creating %s:%u -> %s:%u link\n",
		//	local->name, local_pad->index,
		//	remote->name, remote_pad->index);

		ret = media_create_pad_link(local, local_pad->index,
					       remote, remote_pad->index,
					       link_flags);
		if (ret < 0) {
			dev_err(xdev->dev,
				"failed to create %s:%u -> %s:%u link\n",
				local->name, local_pad->index,
				remote->name, remote_pad->index);
			break;
		}
	}

	return ret;
}


/**
 * xvip_graph_entity_set_streaming - Update the streaming status
 * @xdev: Composite video device
 * @entity: graph entity to update
 * @enable: enable/disable streaming status
 *
 * Update the streaming status of given entity.
 *
 * Return: previous streaming status (true or false)
 */
static bool xvip_graph_entity_set_streaming(struct xvip_composite_device *xdev,
					    struct xvip_graph_entity *entity,
					    bool enable)
{
	bool status = entity->streaming;

	entity->streaming = enable;
	return status;
}

static int xvip_entity_start_stop(struct xvip_composite_device *xdev, struct xvip_graph_entity *entity, bool on)
{
	struct v4l2_subdev *subdev;
	bool is_streaming;
	int ret = 0;

	dev_dbg(xdev->dev, "%s entity %s\n",
		on ? "Starting" : "Stopping", entity->entity->name);
	subdev = media_entity_to_v4l2_subdev(entity->entity);

	/* This is to maintain list of stream on/off devices */
	is_streaming = xvip_graph_entity_set_streaming(xdev, entity, on);

	/*
	 * start or stop the subdev only once in case if they are
	 * shared between sub-graphs
	 */
	if (on && !is_streaming) {
		/* power-on subdevice */
		ret = v4l2_subdev_call(subdev, core, s_power, 1);
		if (ret < 0 && ret != -ENOIOCTLCMD) {
			dev_err(xdev->dev,
				"s_power on failed on subdev\n");
			xvip_graph_entity_set_streaming(xdev, entity, 0);
			return ret;
		}

		/* Check if the device is the IMX274 */
		dev_dbg(xdev->dev, "subdev: (%s)\n", subdev->name);
		if(strcmp(subdev->name,"IMX274") == 0) {
			struct v4l2_fract fract = {
				.numerator = 1,
				.denominator = 60
			};
			struct v4l2_subdev_frame_interval ival = {
				.interval = fract
			};
			dev_dbg(xdev->dev, "Going to change frame interval of subdev: (%s)\n", subdev->name);
			ret = v4l2_subdev_call(subdev, video, s_frame_interval, &ival);
			if (ret < 0) {
				dev_err(xdev->dev,
					"s_frame_interval on failed on subdev\n");
				xvip_graph_entity_set_streaming(xdev, entity, 0);
				return ret;
			}
			dev_dbg(xdev->dev, "Changing frame interval of subdev: (%s) succesfully\n", subdev->name);
		}

		/* stream-on subdevice */
		ret = v4l2_subdev_call(subdev, video, s_stream, 1);
		if (ret < 0 && ret != -ENOIOCTLCMD) {
			dev_err(xdev->dev,
				"s_stream on failed on subdev\n");
			v4l2_subdev_call(subdev, core, s_power, 0);
			xvip_graph_entity_set_streaming(xdev, entity, 0);
		}
	} else if (!on && is_streaming) {
		/* stream-off subdevice */
		ret = v4l2_subdev_call(subdev, video, s_stream, 0);
		if (ret < 0 && ret != -ENOIOCTLCMD) {
			dev_err(xdev->dev,
				"s_stream off failed on subdev\n");
			xvip_graph_entity_set_streaming(xdev, entity, 1);
		}

		/* power-off subdevice */
		ret = v4l2_subdev_call(subdev, core, s_power, 0);
		if (ret < 0 && ret != -ENOIOCTLCMD)
			dev_err(xdev->dev,
				"s_power off failed on subdev\n");
	}

	return ret;
}

static int xvip_graph_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct xvip_graph_entity *entity;
	struct v4l2_async_subdev *asd;
	LIST_HEAD(ent_list);
	int ret;

	dev_dbg(g_xdev->dev, "notify complete, all subdevs registered\n");

	/* Create links for every entity. */
	list_for_each_entry(asd, &g_xdev->notifier.asd_list, asd_list) {
		entity = to_xvip_entity(asd);
		ret = xvip_graph_build_one(g_xdev, entity);
		if (ret < 0)
			return ret;
	}

	dev_dbg(g_xdev->dev, "Going to register v4l2 device \n");

	ret = v4l2_device_register_subdev_nodes(&g_xdev->v4l2_dev);
	if (ret < 0)
		dev_err(g_xdev->dev, "failed to register subdev nodes\n");
	
	return media_device_register(&g_xdev->media_dev);
}

static int xvip_graph_notify_bound(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_subdev *unused)
{
	struct xvip_graph_entity *entity;
	struct v4l2_async_subdev *asd;

	/* Locate the entity corresponding to the bound subdev and store the
	 * subdev pointer.
	 */
	list_for_each_entry(asd, &g_xdev->notifier.asd_list, asd_list) {
		entity = to_xvip_entity(asd);
		if (entity->asd.match.fwnode != subdev->fwnode)
			continue;

		if (entity->subdev) {
			dev_err(g_xdev->dev, "duplicate subdev for node %p\n",
				entity->asd.match.fwnode);
			return -EINVAL;
		}

		//dev_dbg(g_xdev->dev, "subdev %s bound\n", subdev->name);
		entity->entity = &subdev->entity;
		entity->subdev = subdev;
		return 0;
	}

	dev_err(g_xdev->dev, "no entity for subdev %s\n", subdev->name);
	return -EINVAL;
}

static const struct v4l2_async_notifier_operations xvip_graph_notify_ops = {
	.bound = xvip_graph_notify_bound,
	.complete = xvip_graph_notify_complete,
};

static int xvip_graph_parse_one(struct xvip_composite_device *xdev,
				struct fwnode_handle *fwnode)
{
	struct fwnode_handle *remote;
	struct fwnode_handle *ep = NULL;
	int ret = 0;

	//dev_dbg(xdev->dev, "parsing node %pOF\n", node);

	while (1) {
		struct v4l2_async_subdev *asd;
		
		ep = fwnode_graph_get_next_endpoint(fwnode, ep);
		if (ep == NULL)
			break;

		//dev_dbg(xdev->dev, "handling endpoint %pOF\n", ep);

		remote = fwnode_graph_get_remote_port_parent(ep);
		if (remote == NULL) {
			ret = -EINVAL;
			goto err_notifier_cleanup;
		}
		
		fwnode_handle_put(ep);

		/* Skip entities that we have already processed. */
		if (remote == of_fwnode_handle(xdev->dev->of_node) ||
		    xvip_graph_find_entity(xdev, remote)) {
			fwnode_handle_put(remote);
			continue;
		}

		asd = v4l2_async_notifier_add_fwnode_subdev(
			&xdev->notifier, remote,
			sizeof(struct xvip_graph_entity));
		fwnode_handle_put(remote);
		if (IS_ERR(asd)) {
			ret = PTR_ERR(asd);
			goto err_notifier_cleanup;
		}
	}

	return 0;

err_notifier_cleanup:
	v4l2_async_notifier_cleanup(&xdev->notifier);
	fwnode_handle_put(ep);
	return ret;
}

static int xvip_graph_parse(struct xvip_composite_device *xdev)
{
	struct xvip_graph_entity *entity;
	struct v4l2_async_subdev *asd;
	int ret;

	/*
	 * Walk the links to parse the full graph. Start by parsing the
	 * composite node and then parse entities in turn. The list_for_each
	 * loop will handle entities added at the end of the list while walking
	 * the links.
	 */
	ret = xvip_graph_parse_one(xdev, of_fwnode_handle(xdev->dev->of_node));
	if (ret < 0)
		return 0;

	list_for_each_entry(asd, &xdev->notifier.asd_list, asd_list) {
		entity = to_xvip_entity(asd);
		ret = xvip_graph_parse_one(xdev, entity->asd.match.fwnode);
		if (ret < 0) {
			v4l2_async_notifier_cleanup(&xdev->notifier);
			break;
		}
	}

	return ret;
}

static void xvip_graph_cleanup(struct xvip_composite_device *xdev)
{
	v4l2_async_notifier_unregister(&xdev->notifier);
	v4l2_async_notifier_cleanup(&xdev->notifier);
}

static int xvip_graph_init(struct xvip_composite_device *xdev)
{
	int ret;

	/* Parse the graph to extract a list of subdevice DT nodes. */
	ret = xvip_graph_parse(xdev);
	if (ret < 0) {
		dev_err(xdev->dev, "graph parsing failed\n");
		goto done;
	}

	if (list_empty(&xdev->notifier.asd_list)) {
		dev_err(xdev->dev, "no subdev found in graph\n");
		goto done;
	}

	/* Register the subdevices notifier. */
	xdev->notifier.ops = &xvip_graph_notify_ops;

	ret = v4l2_async_notifier_register(&xdev->v4l2_dev, &xdev->notifier);
	if (ret < 0) {
		dev_err(xdev->dev, "notifier registration failed\n");
		goto done;
	}

	ret = 0;

done:
	if (ret < 0)
		xvip_graph_cleanup(xdev);

	return ret;
}

/* -----------------------------------------------------------------------------
 * Media Controller and V4L2
 */

static void xvip_composite_v4l2_cleanup(struct xvip_composite_device *xdev)
{
	v4l2_device_unregister(&xdev->v4l2_dev);
	media_device_unregister(&xdev->media_dev);
	media_device_cleanup(&xdev->media_dev);
}

static int xvip_composite_v4l2_init(struct xvip_composite_device *xdev)
{
	int ret;

	xdev->media_dev.dev = xdev->dev;
	strscpy(xdev->media_dev.model, "Xilinx Video Composite Device",
		sizeof(xdev->media_dev.model));
	xdev->media_dev.hw_revision = 0;

	media_device_init(&xdev->media_dev);

	xdev->v4l2_dev.mdev = &xdev->media_dev;
	ret = v4l2_device_register(xdev->dev, &xdev->v4l2_dev);
	if (ret < 0) {
		dev_err(xdev->dev, "V4L2 device registration failed (%d)\n",
			ret);
		media_device_cleanup(&xdev->media_dev);
		return ret;
	}

	return 0;
}

static void xvip_start_stream(void)
{
	struct xvip_graph_entity *entity;
	struct v4l2_async_subdev *asd;
	dev_dbg(g_xdev->dev, "Starting the stream \n");
	list_for_each_entry(asd, &g_xdev->notifier.asd_list, asd_list) {
		entity = to_xvip_entity(asd);
		xvip_entity_start_stop(g_xdev, entity, true);
	}
	g_xdev->is_streaming = true;
}

static ssize_t xvip_start_stream_show(
	struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", g_xdev->is_streaming);
}

static ssize_t xvip_start_stream_store(
	struct device *dev,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	xvip_start_stream();

	return count;
}

static DEVICE_ATTR(stream_start, S_IRUSR | S_IWUSR, xvip_start_stream_show, xvip_start_stream_store);

static struct attribute *xvip_attrs[] = {
        &dev_attr_stream_start.attr,
        NULL,
};

static const struct attribute_group xvip_attr_group = {
        .attrs = xvip_attrs,
};

/* media_ctl probe and remove */

static int media_ctl_probe(struct platform_device *pdev)
{
	/* For video4Linux */
	int ret;

	g_xdev = devm_kzalloc(&pdev->dev, sizeof(*g_xdev), GFP_KERNEL);
	if (!g_xdev)
		return -ENOMEM;

	g_xdev->dev = &pdev->dev;
	v4l2_async_notifier_init(&g_xdev->notifier);

	ret = xvip_composite_v4l2_init(g_xdev);
	if (ret < 0)
		return ret;

	ret = xvip_graph_init(g_xdev);
	if (ret < 0)
		goto error;

	/* Register attribute */
	ret = sysfs_create_group(&pdev->dev.kobj, &xvip_attr_group);
	if (ret)
		dev_err(&pdev->dev, "sysfs_create_group failed\n");

	return 0;

	/* Error handling v4l */
error:
	xvip_composite_v4l2_cleanup(g_xdev);
	return ret;
}

static int media_ctl_remove(struct platform_device *pdev)
{
	/* Video 4 Linux cleanup */
	struct xvip_composite_device *g_xdev = platform_get_drvdata(pdev);

	xvip_graph_cleanup(g_xdev);
	xvip_composite_v4l2_cleanup(g_xdev);

	return 0;
}


static const struct of_device_id media_ctl_ids[] = {
	{ .compatible = "topic,mediactl" },
	{ },
};
MODULE_DEVICE_TABLE(of, media_ctl_ids);

static struct platform_driver media_ctl_driver = {
	.driver = {
		.name = "topic_mediactl",
		.owner = THIS_MODULE,
		.of_match_table = media_ctl_ids,
	},
	.probe = media_ctl_probe,
	.remove = media_ctl_remove,
};
module_platform_driver(media_ctl_driver);
